# gui/client (`@vehicle-sil/gui-client`)

[日本語](README.ja.md)

Zero-dependency TypeScript client library and custom elements for the `vehicle-sil-gui` server. Runs in browsers and Node.js 22+.

Use this page for onboarding, choosing between `connect()` and `createFront()`, high-level recipes, and embedding the 12 `vsil-*` custom elements across Vanilla JS/TS, Angular, React, Vue, and Svelte. For the low-level `createClient()` API, `variables.bind()`, full export tables, wire-adjacent details, and contributor tests, see [docs/gui-client-low-level.md](../../docs/gui-client-low-level.md).

## Contents

- [Installing and importing](#installing-and-importing)
- [Choosing `connect()`, `createFront()`, or `createClient()`](#choosing-connect-createfront-or-createclient)
- [The front door (`connect()`)](#the-front-door-connect)
- [Recipes](#recipes)
- [Parts (custom elements)](#parts-custom-elements)
- [Framework setup (5 frameworks)](#framework-setup-5-frameworks)
- [Low-level API and see also](#low-level-api-and-see-also)

## Installing and importing

Each release publishes `@vehicle-sil/gui-client` on npm (`latest` for releases, `next` for pre-releases) and attaches `vehicle-sil-gui-client-<version>.tgz` to the release and prebuilt tools archive. Install the same version as your GUI server: the client refuses a server speaking another GUI protocol version.

| Use                                            | How to get it                                                                 | Import                                                       |
| ---------------------------------------------- | ----------------------------------------------------------------------------- | ------------------------------------------------------------ |
| A page served by `vehicle-sil-gui --web`       | Nothing to copy: served by the GUI server under `/vsil/`                      | `import { connect } from '/vsil/vehicle-sil-gui-client.mjs'` |
| Plain JavaScript outside `--web` (or Node 22+) | Copy `vehicle-sil-gui-client.mjs` next to your script                         | `import { connect } from './vehicle-sil-gui-client.mjs'`     |
| TypeScript or any framework build              | `npm install @vehicle-sil/gui-client@<version>` (or `npm install <path>.tgz`) | `import { connect } from '@vehicle-sil/gui-client'`          |

Where to find the built artifacts:

| File                                   | Prebuilt tools                                        | Build in the repository (`gui/web`)                                              |
| -------------------------------------- | ----------------------------------------------------- | -------------------------------------------------------------------------------- |
| `vehicle-sil-gui-client.mjs`           | `<tools>/client/vehicle-sil-gui-client.mjs`           | `npm run build:client` -> `build/gui-client/vehicle-sil-gui-client.mjs`          |
| `vehicle-sil-gui-elements.mjs`         | `<tools>/client/vehicle-sil-gui-elements.mjs`         | `npm run build:client` -> `build/gui-client/vehicle-sil-gui-elements.mjs`        |
| `vehicle-sil-gui-client-<version>.tgz` | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` | `npm run pack:client` -> `build/gui-client/vehicle-sil-gui-client-<version>.tgz` |

Package subpaths (`package.json` `exports`):

| Import path                        | Purpose                                                                                                                                                                                                                     |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `@vehicle-sil/gui-client`          | High-level `connect()` / `createFront()`, low-level `createClient()`, `watch()`, `until()`, `ActionError`, `WriteError`, and protocol types                                                                                 |
| `@vehicle-sil/gui-client/elements` | Registers the 12 `vsil-*` custom elements; exports `pageIntent`, `setReasonText`, element classes, `VsilElementContracts`, and write-event detail types (`WriteQueuedInfo`, `WriteOutcome`, `WriteSuccess`, `WriteFailure`) |
| `@vehicle-sil/gui-client/timeline` | Registers `<vehicle-sil-timeline>` and exports its element and event types                                                                                                                                                  |
| `@vehicle-sil/gui-client/react`    | Type-only JSX intrinsic element types for React 18 and 19 (no runtime dependency on `react`)                                                                                                                                |
| `@vehicle-sil/gui-client/vue`      | Type-only `GlobalComponents` types for Vue 3 (no runtime dependency on `vue`)                                                                                                                                               |
| `@vehicle-sil/gui-client/svelte`   | Type-only `SvelteHTMLElements` types for Svelte 5 (no runtime dependency on `svelte`)                                                                                                                                       |

- **Development server (Vite, `ng serve`, etc.)**: proxy `/api/`, `/vsil/`, and `/ws` to `vehicle-sil-gui` and start the server with `--dev-origin http://127.0.0.1:<port>` (optionally `--no-auth` for local development; see "[Use a development server](../../docs/gui.md#use-a-development-server)" in `docs/gui.md`).

## Choosing `connect()`, `createFront()`, or `createClient()`

| Entry point                                | Connects immediately?                                              | Shared page connection?                            | Best for                                                                                                                                                              |
| ------------------------------------------ | ------------------------------------------------------------------ | -------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `await connect()` (no options, browser)    | Yes (`connect()` + `loadApp()`)                                    | Yes (shared with all `<vsil-*>` parts on the page) | Custom screens served by `--web`, scripts alongside `<vsil-*>` parts                                                                                                  |
| `await connect(options)` (browser or Node) | Yes (`connect()` + `loadApp()`)                                    | No (dedicated connection; call `close()`)          | Node.js scripts/automation, second connection, or explicit `batchMs` / `watchPeriodMs`                                                                                |
| `createFront(options)`                     | No (call `front.low.connect()` and `front.low.loadApp()` yourself) | No (dedicated connection; call `close()`)          | Apps that must render UI before the server answers or while offline (e.g., `gui/web`)                                                                                 |
| `createClient(options)`                    | No (call `client.connect()` and `client.loadApp()` yourself)       | No                                                 | Low-level access to raw session/events/logs, multi-variable atomic writes, or `variables.bind()` ([docs/gui-client-low-level.md](../../docs/gui-client-low-level.md)) |

All three option objects use camelCase keys (`ConnectOptions` is `ClientOptions`):

| Option                      | Type / range                                | Default                                                | Purpose                                                                                                                                                                      |
| --------------------------- | ------------------------------------------- | ------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `baseUrl`                   | `string` (required when options are passed) | Server root in `connect()`                             | GUI server root URL (`http://127.0.0.1:<port>/`, without `#token=...`)                                                                                                       |
| `token`                     | `string \| null \| (() => string \| null)`  | `takeToken(window)` in `connect()`                     | Bearer/WS token from `#token=...`; omit or pass `null` for `--no-auth`                                                                                                       |
| `origin`                    | `string`                                    | Origin of `baseUrl` in Node; unset in browsers/Workers | Override `Origin` header (do not pass in browsers or Web Workers)                                                                                                            |
| `answerTimeoutMs`           | finite `number > 0`                         | `5000` (`ANSWER_TIMEOUT_MS`)                           | Timeout waiting for a time command reply and initial WS sync on `run`/`time`/`write`                                                                                         |
| `batchMs`                   | finite `number` `0..1000`                   | `100` (`BATCH_MS`)                                     | Coalescing interval for incoming WebSocket notifications (`0` flushes on a microtask; control frames `synced`, `operation`, `error`, and `command` always flush immediately) |
| `watchPeriodMs`             | finite `number` `20..60_000`                | `100` (`WATCH_PERIOD_MS`)                              | Default polling interval for variable watches (rounded down to `WATCH_PERIODS_MS` and clamped to server limits)                                                              |
| `http`, `socket`, `onError` | transport / error sink                      | `fetch`, `WebSocket`, `console.error`                  | Custom transports or subscriber error handler                                                                                                                                |

Unknown or legacy snake_case keys (such as `base_url` or `timeout_ms`) are rejected immediately with `ActionError('unknown-field', ..., 'refused')`.

- **Startup retry vs fail-fast**: `connect()` waits for the initial `loadApp()` and WebSocket connection before resolving and **rejects immediately** on connection failure without open-ended startup retries (making script failures obvious). `createFront()` returns immediately and leaves connection retries to your `front.low.connect()` / WebSocket lifecycle so browser UIs can render while offline.

## The front door (`connect()`)

`connect()` returns a `Front`: reactive `Readable` state (`status`, `clock`, `controls`, `scenario`, `runtime`, `outside`), Promise-based actions (`run`, `time`, `check`, `scenarios`, `connections`, `configs`, `recording`, `prepare`, `readConfig`, `describeAgain`, `saveConfig`), and per-variable handles (`variable(ref)`). The underlying low-level client is always available as `front.low`.

```ts
import { ActionError, connect, watch } from '@vehicle-sil/gui-client';

// In a page served by --web, omit options to share the connection with <vsil-*> parts.
// In Node.js, pass { baseUrl, token } (omit token for --no-auth).
const client = await connect();

const stop = watch([client.status, client.controls], (status, controls) => {
  startButton.disabled = !controls.start.ok;
  startButton.title = controls.start.reason?.message ?? '';
  banner.textContent = status.problem?.message ?? '';
});

try {
  await client.time.pause();
  await client.time.step(100); // resolves when time has arrived and the run is paused again
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

Data fields use the wire's snake_case names (`force_stop`, `sim_ms`, `can_write`); methods and JS options use camelCase (`forceStop`, `runUntil`, `timeoutMs`). Every `Readable` returns the same reference while unchanged and has unbound `get` / `subscribe` methods (compatible directly with React `useSyncExternalStore`).

### `status`, `clock`, and `controls`

- **`status`**: `{ link, run, file, result, problem }`.
  - `link`: `'connecting' | 'online' | 'offline' | 'rejected' | 'incompatible'`.
  - `run`: `'none' | 'starting' | 'waiting' | 'running' | 'paused' | 'stopping' | 'ended' | 'crashed'`.
  - `result`: `{ end, scenario, ok, message }` while `run` is `'ended'` or `'crashed'`.
  - `problem`: `{ code, message } | null` — connection errors first, otherwise the newest of launcher crash, failed run, or last failed action (variable write errors appear on `variable.get().problem` instead).
- **`clock`**: `{ sim_ms, speed, actual_speed, pacing }` (`pacing`: `'not-paced' | 'on-time' | 'late'`). Separated from `status` because `sim_ms` updates frequently.
- **`controls`**: `{ start, stop, force_stop, pause, resume, step, run_until, speed, save }`. Each entry is `{ ok, reason, caution }` where `reason` and `caution` are `{ code, message } | null`. Branch on `code` rather than matching the English `message`. For the complete table of every `controls.*.reason.code` and `caution.code` (plus `--time-policy` precedence and `free` default speed), see "[Control reason codes, caution codes, and `--time-policy`](../../docs/gui.md#control-reason-codes-caution-codes-and---time-policy)" in `docs/gui.md`.

### `run`, `time`, and `until()`

- **`run.start(intent?, { file?, onStarted? }?)`**: starts a run. `intent` uses the keys of `schema/run-request.schema.json` (`variant`, `params`, `scenario`, `case`, `after_scenario`, `record`, `record_after_fail_ms`, `disable`, `enable`, `manual`, `strict`, `speed`, `start_paused`, `stop_at_ms`, `late_tolerance_ms`, `connect`, `time_policy`); omit unset keys rather than sending `null`. To re-run the current or last session with the exact same options, pass `session.intent` directly: `await front.run.start(session.intent, { file: session.file })` (`session.intent.scenario` is already root-relative). Resolves once the server accepts the start — wait for `status.run !== 'starting'` with `until()` before reading the new run state.
- **`run.stop()` / `run.forceStop(target?)`**: stops the run. If called right after `await connect()` while `link === 'connecting'`, `run.stop()`, `run.forceStop()`, `time.*()`, and `variable().enqueue()` / `write()` automatically wait for the initial WebSocket sync (up to `answerTimeoutMs`) instead of failing on a not-yet-synced state.
- **`time.pause()` / `resume()` / `step(ms)` / `runUntil(ms)` / `setSpeed(value)`**: `step` and `runUntil` resolve once virtual time reaches the target and the run is paused again; another sender's command before arrival rejects with `ActionError('interrupted', ..., 'unknown')`.
- **`until(source, test, { timeoutMs?, signal? })`**: resolves with the first value matching `test` (checking the current value immediately), or rejects with `ActionError('timeout' | 'aborted', ..., 'unknown')` (including `(last observed: ...)` in timeout errors). Options are strictly camelCase (`timeoutMs`, `signal`); unknown keys reject with `unknown-field`.
- **`scenario` (`timeline_url`) — inspecting completed or `--speed max` runs**: `client.scenario.get()?.timeline_url` (or `session.scenario.timelineUrl`) points to `GET api/timeline/<run_id>` (pass `Authorization: Bearer <token>` unless `--no-auth`). For completed scenario runs and fast `--speed max` runs—where live variable polling skips intermediate steps—fetch the timeline JSON for every recorded virtual-time step sample (the server retains `--keep-runs N` timelines, default 4, keeping failed runs ahead of older passing ones).

### One variable (`client.variable(ref)`)

Pass `"ECU.variable"` (split at the first dot) or `{ peer, name }` (when the ECU name contains a dot).

```ts
const throttle = client.variable('Engine.engine.throttle');

// Subscribe with an optional per-subscriber polling period (20..60_000 ms)
const stop = throttle.subscribe(
  (view) => {
    slider.value = String(view.value ?? 0);
    slider.disabled = !view.can_write;
    slider.title = view.why?.message ?? view.note ?? '';
  },
  { periodMs: 20 },
);

// Single-step write (stages 1-4 combined)
const result = await throttle.write(40);
console.log(result.appliedAtMs, result.late);
stop();
```

**Four-stage write API (`enqueue()` + `applied()` / `write()`)**:

- **`enqueue(value, options?): Promise<QueuedWrite>`**: completes stages 1 + 2 — the GUI server validates the write, registers it in FIFO `seq` order, and waits on the launcher Control Server (`WAIT_FOR_QUEUED`) until the ECU has queued it (or settled it immediately). Rejects immediately with `ActionError` if refused or terminally failed (`invalid`, `busy`, `late`, `rejected`, `cancelled`, ...).
- **`queued.applied(waitOptions?): Promise<WriteResult>`**: completes stages 3 + 4 — resolves with `{ session, seq, peer, name, value, appliedAtMs, late }` once the ECU applies the write in a step, or rejects with `ActionError`.
- **`write(value, options?): Promise<WriteResult>`**: shorthand for `enqueue(value, options).then((q) => q.applied({ timeoutMs, signal }))`. Because `applied()` waits for a virtual-time step, awaiting `write()` directly while `paused` will wait indefinitely until virtual time advances—use `enqueue()` -> `step()` / `runUntil()` -> `applied()` when paused (see Recipe 3 below).
- **`FrontWriteOptions`**: `{ atMs?, late?: 'reject' | 'apply', timeoutMs?, keepalive?, signal? }`. Unknown keys reject immediately with `ActionError('unknown-field', ..., 'refused')`.
- **Virtual-time-aware watchdog (`timeoutMs`)**: default `WRITE_TIMEOUT_MS` (10,000 ms). Must be a finite number `> 0` (`Infinity` is rejected with `invalid-argument`). While the connection is healthy, the target ECU is connected, and the run is `paused` or `holding`, the watchdog is **suspended** (so paused writes and multi-rate scheduled `atMs` writes never false-timeout); pass `signal: AbortSignal.timeout(ms)` if you need a wall-clock cap while paused. While running, the watchdog scales for slow playback (`speed < 1`) and resets as `sim_ms` advances, while still rejecting in finite time if the WebSocket drops, the ECU disconnects, or virtual time freezes.
- **Polling rate and boundary**: the first `subscribe()` starts watching the variable and the last unsubscribe stops it (there is no `close()`). Passing `{ periodMs }` temporarily promotes the variable to the shortest active interval among subscribers and restores the slower interval when that subscriber unsubscribes. Lowering `watchPeriodMs` / `periodMs` to `20` ms (with `batchMs: 16` or `0`) improves live UI responsiveness, but latest-value polling does **not** guarantee capturing pulses shorter than 20 ms; use `--timeline` or `Spy` for deterministic step-accurate capture (see "[Live polling vs deterministic capture](../../docs/gui.md#live-polling-vs-deterministic-capture)" in `docs/gui.md`).
- **Held inputs and reentrancy**: pass `{ keepalive: true }` from a `pagehide` handler for held-control release writes, and avoid triggering unguarded reentrant writes inside `subscribe()`. See "[Held inputs, reconnect recovery, and bus observation](../../docs/gui.md#held-inputs-reconnect-recovery-and-bus-observation)" in `docs/gui.md`.

### Connecting outside and editing configuration files

- **External buses**: call `await client.connections({ network })` when the user opens a target picker (starts that network's bridge on the server host), check an intent with `await client.check({ connect: { ETH_EXT: target }, time_policy: 'free' })`, start with `client.run.start(intent)`, and observe live bridge health on `client.outside`.
- **Configuration text editing**: read saved text and its SHA-256 stamp with `await client.readConfig({ file })`, validate unsaved edits without starting ECUs via `await client.check({ variant }, { file, text })`, refresh cached `--describe` metadata after rebuilding ECUs with `await client.describeAgain({ file, variant })`, and save atomically with `await client.saveConfig({ file, text, stamp })`.

## Recipes

### 1. Discover variables before starting a run (`client.check()`)

`client.check()` runs `vehicle_sil_launch check --json` before any run starts and returns each participant's `--describe` variable metadata, including `builtin` (`true` for automatic variables such as `ecu.connected`, `peer.*`, `j1939.*`, `can.*`, `lin.*`, `eth.*`; `false` for variables declared by your ECU).

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

### 2. Wait through `starting` state after `run.start()`

`client.run.start()` resolves as soon as the server accepts the request, while `status.run` is still `'starting'`. Wait with `until()` using camelCase `timeoutMs`, then inspect `status.run` and `status.result`.

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

### 3. Deterministic writes while paused (`enqueue()` -> `step()` / `runUntil()` -> `applied()`)

Because `enqueue()` waits until the ECU has actually queued the command (`WAIT_FOR_QUEUED`), you never need fixed `sleep()` delays or infinite timeouts before calling `step()` or `runUntil()`.

```ts
import { connect, until } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  await client.run.start({ start_paused: true });
  await until(client.status, (s) => s.run === 'paused', { timeoutMs: 30_000 });

  const throttle = client.variable('Engine.engine.throttle');

  // Queue a rising and falling pulse while paused (zero sleep required)
  const rise = await throttle.enqueue(80, { atMs: 100 });
  const fall = await throttle.enqueue(20, { atMs: 250 });

  // Advance virtual time deterministically to apply both writes
  await client.time.runUntil(300);

  const [r1, r2] = await Promise.all([rise.applied(), fall.applied()]);
  console.log('applied at:', r1.appliedAtMs, r2.appliedAtMs);
} finally {
  await client.run.stop().catch(() => undefined);
  client.close();
}
```

### 4. Record manual writes and replay with handover (`recording()` + `end_ms` + `after_scenario: 'pause'`)

After stopping a run that applied variable writes, `client.recording()` returns scenario YAML (`vehicle_sil_scenario: 1`) with a finalized top-level `end_ms` equal to the recorded run's end virtual time (mid-run calls omit `end_ms` so replay ends at the last `set` step). Save that YAML under `scenarios/` next to the configuration file and replay it with `after_scenario: 'pause'` to inspect or continue manually on the same run without restarting ECUs.

```ts
import { writeFileSync } from 'node:fs';
import { connect, until } from '@vehicle-sil/gui-client';

const client = await connect({ baseUrl: 'http://127.0.0.1:8765/', token });
try {
  // 1. After a recorded run has ended, export its scenario YAML (includes finalized end_ms)
  const rec = await client.recording();
  if (!rec?.yaml || rec.writes === 0) throw new Error('No applied writes recorded');
  console.log(`recorded ${rec.writes} writes, end_ms=${rec.end_ms}`);
  writeFileSync('scenarios/recorded.yaml', rec.yaml, 'utf8');

  // 2. Discover the saved scenario via the server and replay it with handover to manual pause
  const list = await client.scenarios();
  const saved = list.find((s) => s.path.endsWith('recorded.yaml'));
  if (!saved) throw new Error('recorded.yaml not listed under scenarios/');

  await client.run.start({ scenario: saved.path, after_scenario: 'pause' });
  await until(client.scenario, (sc) => sc?.handed_over === true, { timeoutMs: 30_000 });
  console.log('handed over at ms:', client.scenario.get()?.over_at_ms);

  // 3. Writes and time controls are now unlocked on the same run
  const q = await client.variable('Engine.engine.throttle').enqueue(0);
  await client.time.step(100);
  await q.applied();
} finally {
  await client.run.stop().catch(() => undefined);
  client.close();
}
```

### 5. Tune live polling cadence (`watchPeriodMs`, `batchMs`, `subscribe({ periodMs })`)

The default 100 ms polling and 100 ms batching keep background overhead low. For higher-responsiveness gauges or animations, lower `watchPeriodMs` / `batchMs` on `connect()`, or promote only specific variables while subscribed:

```ts
import { connect } from '@vehicle-sil/gui-client';

const client = await connect({
  baseUrl: 'http://127.0.0.1:8765/',
  token,
  watchPeriodMs: 100, // default for most variables
  batchMs: 16, // flush incoming WebSocket frames ~60 Hz
});

// Promote only engine.speed to 20 ms while this panel is mounted
const stop = client.variable('Engine.engine.speed').subscribe(
  (view) => {
    if (view.value !== null) console.log(view.sim_ms, view.text, view.unit);
  },
  { periodMs: 20 },
);
// Calling stop() restores the slower watch period automatically
```

## Parts (custom elements)

`@vehicle-sil/gui-client/elements` defines 12 light-DOM custom elements built on `connect()` with no styles of their own. Every HTML attribute has a matching camelCase JS property (including boolean `noUnit`, falsy `value={false}` / `value={0}`, and numeric `stepMs`) so they bind cleanly in plain HTML and in Angular, React, Vue, and Svelte templates.

| Element                  | What it shows or does                                                                                                                   | Attributes / JS properties                                                                                                                                                     | Custom events                            |
| ------------------------ | --------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------- |
| `<vsil-status>`          | Connection, run state, virtual time (marked when late), and active problem                                                              | `client` (prop)                                                                                                                                                                | —                                        |
| `<vsil-run-controls>`    | Start (with page intent), Stop, and Force stop                                                                                          | `label-start` (`labelStart`), `label-stop` (`labelStop`), `label-force-stop` (`labelForceStop`), `intent` (prop), `client` (prop)                                              | —                                        |
| `<vsil-time-controls>`   | Pause, Resume, Step, Run until, and speed selector                                                                                      | `speeds`, `step-ms` (`stepMs`), `label-pause` (`labelPause`), `label-resume` (`labelResume`), `label-step` (`labelStep`), `label-run-until` (`labelRunUntil`), `client` (prop) | —                                        |
| `<vsil-value>`           | Value with unit and non-live note; bool renders `on`/`off` text                                                                         | `variable`, `on`, `off`, `no-unit` (`noUnit`), `client` (prop)                                                                                                                 | —                                        |
| `<vsil-slider>`          | Numeric slider in variable's range; writes on release with single-slot coalescing                                                       | `variable`, `client` (prop)                                                                                                                                                    | `vsil-write-queued`, `vsil-write-result` |
| `<vsil-write>`           | Button writing one value (disabled when variable already equals value)                                                                  | `variable`, `value` (`string \| number \| boolean \| null`), `label`, `client` (prop)                                                                                          | `vsil-write-queued`, `vsil-write-result` |
| `<vsil-scenario-select>` | Scenario selector for the next run                                                                                                      | `client` (prop)                                                                                                                                                                | —                                        |
| `<vsil-case-select>`     | Case selector for the next run's scenario when it defines multiple cases (disables cases restricted by the active `variant` via `only`) | `client` (prop)                                                                                                                                                                | —                                        |
| `<vsil-scenario-result>` | Scenario status, case, resolved parameters, and step table (including failed step `file:line` and template `call_stack`)                | `client` (prop)                                                                                                                                                                | —                                        |
| `<vsil-timeline>`        | Ties an inner `<vehicle-sil-timeline>` to the run's `timeline_url`                                                                      | `client` (prop)                                                                                                                                                                | —                                        |
| `<vsil-connect>`         | External bus target picker (`SIL Kit only` + bridge targets)                                                                            | `network`, `file`, `client` (prop)                                                                                                                                             | —                                        |
| `<vsil-outside>`         | Live rows of externally connected networks, condition, and frame counts                                                                 | `client` (prop)                                                                                                                                                                | —                                        |

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

`<vsil-timeline>` only sets `src` and `hidden` on the `<vehicle-sil-timeline>` inside it (adding a default element if you omit one) and sets `data-timeline` (`shown` or `none`). Load the timeline script separately (`<script src="/timeline/vehicle-sil-timeline.js">` or `import '@vehicle-sil/gui-client/timeline'`).

```ts
import '@vehicle-sil/gui-client/elements'; // defines the 12 parts
import { pageIntent, setReasonText } from '@vehicle-sil/gui-client/elements';

setReasonText((reason) => (reason.code === 'busy' ? 'Wait a moment.' : null));
pageIntent.update({ variant: 'winter' }); // applied to the next Start alongside <vsil-scenario-select>
const controls = document.querySelector('vsil-run-controls'); // typed via HTMLElementTagNameMap
if (controls) {
  controls.intent = { speed: 'max' };
  controls.labelStart = 'Run';
}
```

### What the parts promise

- **Shared page connection**: by default every part calls `connect()` without options, sharing one WebSocket with your script. Set `element.client = front` to bind a part to another `Front`. Never mix `/vsil/` files with local copies of the library (a second module copy opens a second connection).
- **Light DOM and `data-*` styling hooks**: parts draw inside a `<span class="vsil">` with no shadow DOM. Style them via `[aria-disabled="true"]`, `[data-link]`, `[data-run]`, `[data-pacing]`, `[data-stale="true"]`, `[data-can-write]`, `[data-availability="inactive"]`, `[data-on]`, `[data-error]`, `[data-cases]` (`<vsil-case-select>`: `none` or `available`), `[data-status]` and `[data-case]` (`<vsil-scenario-result>`, plus `data-status` on its steps), `[data-reason]`, `[data-caution]`, and `[data-write-status]`.
- **Write lifecycle events (`vsil-write-queued` & `vsil-write-result`)**: `<vsil-slider>` and `<vsil-write>` dispatch two bubbling, composed (`bubbles: true, composed: true`) lowercase custom events (never fired after disconnect or rebind):
  - `vsil-write-queued`: `CustomEvent<WriteQueuedInfo>` (`{ session, seq, peer, name, value }`) when the write reaches the ECU queue.
  - `vsil-write-result`: `CustomEvent<WriteOutcome>` — either `WriteSuccess` (`{ ok: true, session, seq, peer, name, value, appliedAtMs, late }`) or `WriteFailure` (`{ ok: false, peer, name, value, error: ActionError, seq? }`).
  - Both elements also reflect the latest initiated write lifecycle on `data-write-status` (`idle`, `sending`, `queued`, `applied`, `failed`), resetting automatically across run boundaries.
- **Accessible, uncluttered reason display (`.vsil-why`, `.vsil-note`, `QUIET`)**: disabled buttons stay focusable with `aria-disabled="true"`, `data-reason`, and an `aria-describedby` link to `<small class="vsil-why">` (or `<small class="vsil-note">` on variable parts). Empty descriptions and self-evident `QUIET` codes (`not-running`, `offline`, `connecting`, `already-running`, `paused`, `not-paused`, `not-stopping`, `already-stopping`, `starting`, plus `already` on `<vsil-write>`) set `hidden` so they stay readable to screen readers without cluttering layout. When styling `.vsil-why` or `.vsil-note` in CSS, target `:not([hidden])` so your `display` rule does not unhide hidden notes. See "[Styling parts and subscribing to write events](../../docs/gui.md#styling-parts-and-subscribing-to-write-events)" in `docs/gui.md`.
- **Failure deduplication**: if the shared connection fails (e.g., missing token), every part gets `data-error` while the error sentence appears once (`<vsil-status>` first, otherwise the first part in document order).

### Words and the next run

`setReasonText(fn)` replaces English reason sentences (`fn(reason)` returns a string, or `null`/`undefined` to keep English; `setReasonText(null)` resets). Call it before inserting parts. Built-in part labels (such as `ready`, `writing…`, `(no scenario)`, `(no cases)`, `(choose a case)`, `fastest`, `behind real time`) are fixed English strings. `pageIntent` (`get()`, `subscribe(fn)`, `update(patch)`) stores the intent for the **next** run (never overwritten by the currently running session): `<vsil-scenario-select>` writes `scenario` and clears `case` when the scenario changes; `<vsil-case-select>` reads `scenario` and `variant` from `pageIntent`, checks the scenario to list its cases, and writes `case`; `<vsil-run-controls>` merges `{ ...pageIntent.get(), ...controls.intent }`.

### Connecting a network outside with the parts

`<vsil-connect network="ETH_EXT">` fetches `connections()` lazily when opened (never on page load or Tab focus), puts the choice into `pageIntent.connect`, debounces `check()` (~200 ms), and reverts unlisted targets to `none` with `data-reverted`. Set `time_policy` from script via `pageIntent.update({ time_policy: 'free' })`. `<vsil-outside>` renders `client.outside` rows with `data-network`, `data-condition`, and `data-nothing-received`.

## Framework setup (5 frameworks)

All 5 environments below are compiled and smoke-tested against the real packed `.tgz` in CI (`npm --prefix gui/web run verify:consumers`).

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

Add `CUSTOM_ELEMENTS_SCHEMA` to the component and import `@vehicle-sil/gui-client/elements` (and `@vehicle-sil/gui-client/timeline` if using `<vehicle-sil-timeline>`):

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

### React 18 and 19

Import `@vehicle-sil/gui-client/react` once (for example in `src/main.tsx` or `src/env.d.ts`) to register JSX types for all 12 `vsil-*` parts and `<vehicle-sil-timeline>`.

```tsx
import { useEffect, useRef } from 'react';
import type { Front } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/react';
import '@vehicle-sil/gui-client/timeline';
import type { VsilSliderElement, WriteOutcome } from '@vehicle-sil/gui-client/elements';

// React 19: lowercase `on<event>` JSX props delegate directly via addEventListener.
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

// React 18 (also works in React 19): attach custom element listeners and object props via ref + useEffect.
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

> **React 19 `onload` / `onerror` note**: React intercepts PascalCase `onLoad` / `onError` as synthetic React events that never fire for custom-element `CustomEvent`s (our `./react` types omit `onLoad` / `onError` on `<vehicle-sil-timeline>` to catch this at compile time). Use lowercase `onload` / `onerror` in React 19, or `ref` + `addEventListener` when supporting both React 18 and React 19.

### Vue 3

Configure Vue's template compiler so `vsil-*` and `vehicle-sil-timeline` are treated as native custom elements (for example in `vite.config.ts`: `vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('vsil-') || tag === 'vehicle-sil-timeline' } } })`), and import `@vehicle-sil/gui-client/vue` for `vue-tsc` types:

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

Import `@vehicle-sil/gui-client/svelte` for `svelte-check` element and event typing:

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

## Low-level API and see also

| To find                                                                                                                                                        | Document                                                                    |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| Low-level `createClient()`, full `Public API`, `variables.bind()`, `createWaveforms()`, `createVariableEditor()`, `raw` keys, lifecycle, and contributor tests | [docs/gui-client-low-level.md](../../docs/gui-client-low-level.md)          |
| Control `reason.code` / `caution.code` tables, CSP restrictions, live polling vs `--timeline`/`Spy`, and screen rules                                          | [docs/gui.md](../../docs/gui.md)                                            |
| Step-by-step guide to build and serve your own screen                                                                                                          | [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) |
| Server HTTP and WebSocket protocol specification                                                                                                               | [gui/protocol/README.md](../protocol/README.md)                             |
| No-build HTML/CSS/JS screen starter template                                                                                                                   | [examples/gui_starter](../../examples/gui_starter/)                         |
| Node.js headless client examples (`run.mjs`, `front.mjs`, `variables.mjs`)                                                                                     | [examples/gui_client](../../examples/gui_client/)                           |
