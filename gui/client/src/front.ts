// The front door of the client: `connect()` returns it. It is built from `client.low` (the client that
// `createClient` makes; the same object is `front.low`), the launcher's `state` and the GUI server's `allowed`. It adds
// to the low client and changes nothing in it. The rules of what it shows are in front-compose.ts, the operations in
// front-actions.ts and the variable in front-variable.ts.
import { ClientOptions, buildClient } from './client';
import { ANSWER_TIMEOUT_MS } from './time';
import { takeToken } from './token';
import type { HttpTransport } from './http';
import type { SocketFactory } from './connection';
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
} from './front-compose';
import { FrontActions, actionError, noFileError } from './front-actions';
import { entriesOf, outsideRows } from './front-outside';
import { makeVariable, refOf } from './front-variable';
import { ErrorSink, Readable, derived } from './values';
import { linkOf } from './front-compose';

/** Options of `connect()`. In the browser, call it without options (the page's own server and token are used). */
export interface ConnectOptions {
  /** URL of the GUI server (what the server printed, without `#token=...`). */
  base_url: string;
  /** The token (what follows `#token=`). A function is called each time the token is used. */
  token: string | (() => string | null);
  /** Origin to send (Node sends the origin of `base_url` if omitted). */
  origin?: string;
  /** HTTP transport (default: fetch). For tests. */
  http?: HttpTransport;
  /** Function that opens the WebSocket (default: the platform's). For tests. */
  socket?: SocketFactory;
  /** How long to wait for the launcher's answer to a time command (ms; default 5000). */
  answer_timeout_ms?: number;
  /** Receives what subscribers threw (default: console.error). */
  on_error?: ErrorSink;
}

function makeFront(options: ClientOptions, onClose: () => void): Front {
  const { client: low, http, onError } = buildClient(options);

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
    busy: actions.busy.get(),
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
    snapshot,
    controls: () => controls.get(),
    run,
    initialFile,
    answerTimeoutMs: options.answerTimeoutMs ?? ANSWER_TIMEOUT_MS,
    onError,
    stamp,
  });
  const controlsValue = derived(
    [...sources, actions.busy],
    () => {
      const s = snapshot();
      return controlsOf(s, runOf(s));
    },
    sameData,
    onError,
  );

  const writeControl = derived(
    [...sources, actions.busy],
    () => {
      const s = snapshot();
      return writeOf(s, runOf(s));
    },
    sameData,
    onError,
  );

  const status: Readable<FrontStatus> = derived(
    [...sources, actions.busy, actions.failure],
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
            status: view.status,
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
    outside: outside.value,
    connections: ({ network }, listOptions = {}) => {
      const file = listOptions.file ?? initialFile();
      if (file === null) {
        return Promise.reject(noFileError());
      }
      return low.connections({ file, network }).then(entriesOf, (error: unknown) => {
        throw actionError(error);
      });
    },
    check: (intent, checkOptions) => actions.check(intent, checkOptions),
    readConfig: (readOptions) => actions.readConfig(readOptions),
    describeAgain: (describeOptions) => actions.describeAgain(describeOptions),
    run: {
      start: (intent, startOptions) => actions.start(intent, startOptions),
      stop: () => actions.stop(),
      forceStop: () => actions.forceStop(),
    },
    time: {
      pause: () => actions.pause(),
      resume: () => actions.resume(),
      step: (ms) => actions.step(ms),
      setSpeed: (value) => actions.setSpeed(value),
      runUntil: (ms) => actions.runUntil(ms),
    },
    variable: (ref) => {
      const named = refOf(ref);
      const key = `${named.peer}\u0000${named.name}`;
      let variable = variables.get(key);
      if (!variable) {
        variable = makeVariable(
          low,
          named,
          () => {
            const link = linkOf(snapshot());
            return link === 'rejected' || link === 'incompatible'
              ? new ActionError(link, writeControl.get().reason?.message ?? link, 'refused')
              : null;
          },
          writeControl,
        );
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
  const front = makeFront(
    {
      baseUrl: options.base_url,
      token: options.token,
      origin: options.origin,
      http: options.http,
      socket: options.socket,
      answerTimeoutMs: options.answer_timeout_ms,
      onError: options.on_error,
    },
    onClose,
  );
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

/** Where this page was opened from, in the browser (the server's root and the token of the URL it printed). */
function pageOptions(): ConnectOptions {
  const page = (globalThis as { window?: Window }).window;
  const token = page ? takeToken(page) : null;
  if (!page || token === null) {
    throw new ActionError(
      'no-token',
      'connect() without options works in a page that the GUI server served and that was opened from the URL it printed (it ends with #token=...). In Node, pass { base_url, token }',
      'refused',
    );
  }
  return { base_url: new URL('/', page.location.href).href, token };
}

/**
 * Connects to the GUI server and returns the front door. Without options (in the browser) it connects once and returns
 * the same connection to every caller: the parts of a page share it. With options it makes a connection of its own
 * (close it with `close()`). A failure to connect is not remembered: the next call connects again.
 */
export function connect(options?: ConnectOptions): Promise<Front> {
  if (options) return open(options, () => undefined);
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
 * Resolves with the first value that satisfies `test` (the current one counts). Rejects with an ActionError `timeout`
 * after `timeout_ms`, or `aborted` when `signal` aborts (`unknown`: the thing waited for may still happen).
 */
export function until<T>(
  source: Readable<T>,
  test: (value: T) => boolean,
  options: { timeout_ms?: number; signal?: AbortSignal } = {},
): Promise<T> {
  return new Promise<T>((resolve, reject) => {
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
    const check = (value: T) => {
      if (test(value)) finish(() => resolve(value));
    };
    cleanups.push(source.subscribe(check));
    options.signal?.addEventListener('abort', onAbort);
    if (options.timeout_ms !== undefined) {
      timer = setTimeout(
        () =>
          finish(() =>
            reject(
              new ActionError(
                'timeout',
                `the condition did not hold within ${options.timeout_ms! / 1000} s: look at the state before you wait again`,
                'unknown',
              ),
            ),
          ),
        options.timeout_ms,
      );
    }
    const first = source.get();
    if (test(first)) finish(() => resolve(first));
  });
}
