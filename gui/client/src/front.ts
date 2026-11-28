// The front door of the client: `connect()` returns it. It is built from `client.low` (the client that
// `createClient` makes; the same object is `front.low`), the launcher's `state` and the GUI server's `allowed`. It adds
// to the low client and changes nothing in it. The rules of what it shows are in front-compose.ts, the operations in
// front-actions.ts and the variable in front-variable.ts.
import { ClientOptions, buildClient } from './client';
import { ANSWER_TIMEOUT_MS } from './time';
import { takeToken } from './token';
import {
  ActionError,
  Front,
  FrontClock,
  FrontControls,
  FrontScenario,
  FrontStatus,
} from './front-types';
import {
  Candidate,
  Snapshot,
  clockOf,
  controlsOf,
  writeOf,
  linkProblem,
  newest,
  runOf,
  sameData,
  statusOf,
  linkOf,
} from './front-compose';
import { FrontActions, actionError, noFileError } from './front-actions';
import { entriesOf, outsideRows } from './front-outside';
import { makeVariable, refOf, validateFrontWriteOptions } from './front-variable';
import { Readable, derived } from './values';

/** Options of `connect()`: the same camelCase options as `createFront()` and `createClient()`. */
export type ConnectOptions = ClientOptions;

/** Options of `until()`. */
export interface UntilOptions {
  /** Maximum wait duration in milliseconds before rejecting with `ActionError('timeout', ..., 'unknown')`. */
  timeoutMs?: number;
  /** Signal to abort the wait with `ActionError('aborted', ..., 'unknown')`. */
  signal?: AbortSignal;
}

const UNTIL_OPTION_FIELDS = new Set(['timeoutMs', 'signal']);
const TIMER_MAX_MS = 2_147_483_647;

function makeFront(options: ClientOptions, onClose: () => void): Front {
  const { client: low, http, onError, runControl } = buildClient(options);

  let counter = 0;
  const stamp = () => ++counter;
  const stamps = new Map<string, number>();
  const stampFor = (key: string) => {
    let value = stamps.get(key);
    if (value === undefined) stamps.set(key, (value = stamp()));
    return value;
  };

  const initialFile = () => low.app.get()?.initial_file ?? null;
  // Late: the operations are made first, and the values below read them
  // eslint-disable-next-line prefer-const
  let actions: FrontActions;
  const snapshot = (): Snapshot => ({
    connection: low.connection.state.get(),
    failures: low.connection.failures.get(),
    closedBefore: low.connection.lastClose.get() !== null,
    rejected: low.session.rejected.get(),
    incompatible: low.session.incompatible.get(),
    ready: low.session.ready.get(),
    session: low.session.session.get(),
    allowed: low.session.allowed.get(),
    limits: low.limits.get(),
    busy: actions.busyForCurrent(),
    awaitingRunSession: actions.isAwaitingRunSession(),
    stopConfirmed: actions.isStopConfirmed(),
  });

  const sources: Readable<unknown>[] = [
    low.connection.state,
    low.connection.failures,
    low.connection.lastClose,
    low.session.rejected,
    low.session.incompatible,
    low.session.ready,
    low.session.session,
    low.session.allowed,
    low.limits,
    low.app,
  ];

  const controls: Readable<FrontControls> = {
    get: () => controlsValue.get(),
    subscribe: (listener) => controlsValue.subscribe(listener),
  };
  const run = () => runOf(snapshot());
  actions = new FrontActions({
    low,
    http,
    captureStop: () => runControl.captureStop(),
    activeTarget: () => runControl.activeTarget(),
    snapshot,
    controls: () => controls.get(),
    run,
    initialFile,
    answerTimeoutMs: options.answerTimeoutMs ?? ANSWER_TIMEOUT_MS,
    onError,
    stamp,
  });
  const controlsValue = derived(
    [...sources, actions.busy, actions.startedRunId, actions.stopConfirmedFor],
    () => {
      const s = snapshot();
      return controlsOf(s, runOf(s));
    },
    sameData,
    onError,
  );

  const writeControl = derived(
    [...sources, actions.busy, actions.startedRunId, actions.stopConfirmedFor],
    () => {
      const s = snapshot();
      return writeOf(s, runOf(s));
    },
    sameData,
    onError,
  );

  const status: Readable<FrontStatus> = derived(
    [...sources, actions.busy, actions.startedRunId, actions.stopConfirmedFor, actions.failure],
    () => {
      const s = snapshot();
      const base = statusOf(s, initialFile());
      let problem = linkProblem(s);
      if (!problem) {
        const id = s.session?.id;
        const candidates: Candidate[] = [];
        if (base.run === 'crashed' && base.result) {
          candidates.push({
            stamp: stampFor(`crashed:${id}`),
            problem: { code: 'launcher-crashed', message: base.result.message },
          });
        }
        if (base.run === 'ended' && base.result && !base.result.ok) {
          candidates.push({
            stamp: stampFor(`failed:${id}`),
            problem: { code: 'run-failed', message: base.result.message },
          });
        }
        const failure = actions.failure.get();
        if (failure && failure.run === base.run) {
          candidates.push({
            stamp: failure.stamp,
            problem: { code: failure.error.code, message: failure.error.message },
          });
        }
        problem = newest(candidates);
      }
      return { ...base, problem };
    },
    sameData,
    onError,
  );

  const clock: Readable<FrontClock> = derived(
    sources,
    () => clockOf(snapshot()),
    sameData,
    onError,
  );

  const scenario: Readable<FrontScenario | null> = derived(
    [low.session.session],
    () => {
      const view = low.session.session.get()?.scenario;
      return view
        ? {
            file: view.file,
            name: view.name,
            ...(view.case === undefined ? {} : { case: view.case }),
            ...(view.params === undefined ? {} : { params: view.params }),
            status: view.status,
            handed_over: view.handedOver,
            over_at_ms: view.overAtMs,
            steps: view.steps,
            timeline_url: view.timelineUrl,
          }
        : null;
    },
    sameData,
    onError,
  );

  const outside = outsideRows(low, onError);

  const variables = new Map<string, ReturnType<typeof makeVariable>>();
  let closed = false;

  return {
    low,
    status,
    clock,
    controls,
    scenario,
    runtime: low.session.runtime,
    outside: outside.value,
    prepare: (prepareOptions = {}) => {
      const file = prepareOptions.file ?? initialFile();
      if (file === null) {
        return Promise.reject(noFileError());
      }
      return low
        .prepare({
          file,
          ...(prepareOptions.variant !== undefined ? { variant: prepareOptions.variant } : {}),
          ...(prepareOptions.disable !== undefined ? { disable: prepareOptions.disable } : {}),
          ...(prepareOptions.enable !== undefined ? { enable: prepareOptions.enable } : {}),
          ...(prepareOptions.manual !== undefined ? { manual: prepareOptions.manual } : {}),
        })
        .then(
          (res) => res.runtime ?? low.session.runtime.get(),
          (error: unknown) => {
            throw actionError(error);
          },
        );
    },
    configs: () =>
      low.configs().catch((error: unknown) => {
        throw actionError(error);
      }),
    recording: () =>
      low.recording().catch((error: unknown) => {
        throw actionError(error);
      }),
    connections: ({ network }, listOptions = {}) => {
      const file = listOptions.file ?? initialFile();
      if (file === null) {
        return Promise.reject(noFileError());
      }
      return low.connections({ file, network }).then(entriesOf, (error: unknown) => {
        throw actionError(error);
      });
    },
    scenarios: (listOptions = {}) => {
      const file = listOptions.file ?? initialFile();
      if (file === null) {
        return Promise.reject(
          new ActionError(
            'no-file',
            'no configuration file was given and the server has none to start with: pass { file } (a path from client.low.configs())',
            'refused',
          ),
        );
      }
      return low.scenarios(file).then(
        (list) => list.scenarios,
        (error: unknown) => {
          throw actionError(error);
        },
      );
    },
    check: (intent, checkOptions) => actions.check(intent, checkOptions),
    readConfig: (readOptions) => actions.readConfig(readOptions),
    describeAgain: (describeOptions) => actions.describeAgain(describeOptions),
    saveConfig: (saveOptions) => actions.saveConfig(saveOptions),
    run: {
      start: (intent, startOptions) => actions.start(intent, startOptions),
      stop: () => actions.stop(),
      forceStop: (target) => actions.forceStop(target),
    },
    time: {
      pause: (target) => actions.pause(target),
      resume: (target) => actions.resume(target),
      step: (ms, target) => actions.step(ms, target),
      setSpeed: (value, target) => actions.setSpeed(value, target),
      runUntil: (ms, target) => actions.runUntil(ms, target),
    },
    variable: (ref) => {
      const named = refOf(ref);
      const key = `${named.peer}\u0000${named.name}`;
      let variable = variables.get(key);
      if (!variable) {
        const base = makeVariable(
          low,
          named,
          () => {
            const s = snapshot();
            const link = linkOf(s);
            if (link === 'rejected' || link === 'incompatible') {
              return new ActionError(link, writeControl.get().reason?.message ?? link, 'refused');
            }
            if (link === 'offline' && low.session.currentInstance() === null) {
              const why = linkProblem(s);
              return new ActionError('offline', why?.message ?? 'offline', 'refused');
            }
            return null;
          },
          writeControl,
        );
        const enqueueWithSync = (
          value: number | boolean | string,
          writeOptions?: Parameters<typeof base.enqueue>[1],
        ) => {
          const bad = validateFrontWriteOptions(writeOptions);
          if (bad) return Promise.reject(bad);
          if (writeOptions?.signal?.aborted) {
            return Promise.reject(
              new ActionError('aborted', 'the write was aborted before it was sent', 'refused'),
            );
          }
          if (linkOf(snapshot()) !== 'connecting') {
            return base.enqueue(value, writeOptions);
          }
          const signal = writeOptions?.signal;
          const waitSync = signal
            ? new Promise<void>((resolve, reject) => {
                const onAbort = () =>
                  reject(
                    new ActionError(
                      'aborted',
                      'the write was aborted before it was sent',
                      'refused',
                    ),
                  );
                signal.addEventListener('abort', onAbort, { once: true });
                actions.waitForSync('write', false).then(
                  () => {
                    signal.removeEventListener('abort', onAbort);
                    resolve();
                  },
                  (error: unknown) => {
                    signal.removeEventListener('abort', onAbort);
                    reject(error);
                  },
                );
              })
            : actions.waitForSync('write', false);
          return waitSync.then(() => base.enqueue(value, writeOptions));
        };
        variable = {
          get: base.get,
          subscribe: base.subscribe,
          enqueue: enqueueWithSync,
          write: (value, writeOptions) =>
            enqueueWithSync(value, writeOptions).then((queued) =>
              queued.applied({
                ...(writeOptions?.timeoutMs !== undefined
                  ? { timeoutMs: writeOptions.timeoutMs }
                  : {}),
                ...(writeOptions?.signal !== undefined ? { signal: writeOptions.signal } : {}),
              }),
            ),
        };
        variables.set(key, variable);
      }
      return variable;
    },
    close: () => {
      if (closed) return;
      closed = true;
      outside.stop();
      actions.dispose();
      low.close();
      onClose();
    },
  };
}

/** The connection that `connect()` without options shares (a promise, so that two callers at once get one connection). */
let shared: Promise<Front> | null = null;

async function open(options: ConnectOptions, onClose: () => void): Promise<Front> {
  const front = makeFront(options, onClose);
  front.low.connect();
  try {
    await front.low.loadApp();
  } catch (error) {
    front.close();
    throw actionError(error);
  }
  return front;
}

/**
 * Makes the front door without connecting and without waiting for the server (as `createClient` does for the client
 * below it). For an app that must show its screens before the server answers, and while it cannot be reached: it calls
 * `front.low.connect()` and `front.low.loadApp()` itself, and reads `status` (the connection, `rejected`, `incompatible`)
 * meanwhile. Until `loadApp()` has answered, the front knows no start-up file, and the ranges in `controls` are the
 * defaults (`DEFAULT_LIMITS`).
 * Most code wants `connect()`.
 */
export function createFront(options: ClientOptions): Front {
  return makeFront(options, () => undefined);
}

/** Where this page was opened from, in the browser (the server's root and, if present, the token of the URL it printed). */
function pageOptions(): ConnectOptions {
  const page = (globalThis as { window?: Window }).window;
  if (!page) {
    throw new ActionError(
      'no-token',
      'connect() without options works in a page that the GUI server served. In Node, pass { baseUrl } (and token unless the server runs with --no-auth)',
      'refused',
    );
  }
  return { baseUrl: new URL('/', page.location.href).href, token: takeToken(page) };
}

/**
 * Connects to the GUI server and returns the front door. Without options (in the browser) it connects once and returns
 * the same connection to every caller: the parts of a page share it. With options it makes a connection of its own
 * (close it with `close()`). A failure to connect is not remembered: the next call connects again.
 */
export function connect(options?: ConnectOptions): Promise<Front> {
  if (options !== undefined) return open(options, () => undefined);
  if (shared) return shared;
  let promise: Promise<Front>;
  try {
    promise = open(pageOptions(), () => {
      if (shared === promise) shared = null;
    });
  } catch (error) {
    return Promise.reject(error);
  }
  shared = promise;
  promise.catch(() => {
    if (shared === promise) shared = null;
  });
  return promise;
}

/** Calls `fn` now and each time the value changes. Returns the function that stops it. */
export function watch<T>(source: Readable<T>, fn: (value: T) => void): () => void;
export function watch<T extends unknown[]>(
  sources: { [K in keyof T]: Readable<T[K]> },
  fn: (...values: T) => void,
): () => void;
export function watch(
  source: Readable<unknown> | readonly Readable<unknown>[],
  fn: (...values: never[]) => void,
): () => void {
  const list = Array.isArray(source)
    ? (source as readonly Readable<unknown>[])
    : [source as Readable<unknown>];
  const call = fn as (...values: unknown[]) => void;
  const run = () => call(...list.map((s) => s.get()));
  const stops = list.map((s) => s.subscribe(run));
  run();
  return () => stops.forEach((stop) => stop());
}

/**
 * Formats the most recently observed `until()` value concisely for timeout error messages.
 */
function describeObservedValue(value: unknown): string {
  if (value === undefined) return 'undefined';
  if (value === null) return 'null';
  if (typeof value === 'string') {
    const shown = JSON.stringify(value);
    return shown.length > 200 ? `${shown.slice(0, 197)}..."` : shown;
  }
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint') {
    return String(value);
  }
  if (value instanceof Error) {
    return value.message || value.name;
  }
  try {
    const seen = new WeakSet<object>();
    const json = JSON.stringify(value, (_key, item: unknown) => {
      if (typeof item === 'bigint') return item.toString();
      if (typeof item === 'object' && item !== null) {
        if (seen.has(item)) return '[Circular]';
        seen.add(item);
      }
      return item;
    });
    if (json !== undefined) {
      return json.length > 200 ? `${json.slice(0, 197)}...` : json;
    }
  } catch {
    // Fall back to String(value) if a custom getter/toJSON throws.
  }
  const text = String(value);
  return text.length > 200 ? `${text.slice(0, 197)}...` : text;
}

/**
 * Resolves with the first value that satisfies `test` (the current one counts). Rejects with an ActionError `timeout`
 * after `timeoutMs`, or `aborted` when `signal` aborts (`unknown`: the thing waited for may still happen).
 */
export function until<T>(
  source: Readable<T>,
  test: (value: T) => boolean,
  options: UntilOptions = {},
): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    if (typeof options !== 'object' || options === null || Array.isArray(options)) {
      reject(
        new ActionError('invalid-argument', 'the options of until must be an object', 'refused'),
      );
      return;
    }
    const unknown = Object.keys(options).filter((k) => !UNTIL_OPTION_FIELDS.has(k));
    if (unknown.length > 0) {
      reject(
        new ActionError(
          'unknown-field',
          `the options of until have ${unknown.join(', ')}, which until does not know: the fields are ${[...UNTIL_OPTION_FIELDS].join(', ')}`,
          'refused',
        ),
      );
      return;
    }
    if (
      options.timeoutMs !== undefined &&
      (typeof options.timeoutMs !== 'number' ||
        !Number.isFinite(options.timeoutMs) ||
        options.timeoutMs < 0)
    ) {
      reject(
        new ActionError(
          'invalid-argument',
          'options.timeoutMs must be a finite number of milliseconds, 0 or more',
          'refused',
        ),
      );
      return;
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    const cleanups: (() => void)[] = [];
    const finish = (done: () => void) => {
      clearTimeout(timer);
      cleanups.forEach((cleanup) => cleanup());
      options.signal?.removeEventListener('abort', onAbort);
      done();
    };
    const onAbort = () =>
      finish(() => reject(new ActionError('aborted', 'the wait was aborted', 'unknown')));
    if (options.signal?.aborted) {
      reject(new ActionError('aborted', 'the wait was aborted', 'unknown'));
      return;
    }
    let last: T = source.get();
    const check = (value: T) => {
      last = value;
      if (test(value)) finish(() => resolve(value));
    };
    cleanups.push(source.subscribe(check));
    options.signal?.addEventListener('abort', onAbort);
    if (options.timeoutMs !== undefined) {
      const timeoutMs = options.timeoutMs;
      timer = setTimeout(
        () =>
          finish(() =>
            reject(
              new ActionError(
                'timeout',
                `the condition did not hold within ${timeoutMs / 1000} s (last observed: ${describeObservedValue(last)}): look at the state before you wait again`,
                'unknown',
              ),
            ),
          ),
        Math.min(timeoutMs, TIMER_MAX_MS),
      );
    }
    const first = source.get();
    last = first;
    if (test(first)) finish(() => resolve(first));
  });
}
