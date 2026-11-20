import { ClientConnection, Connection, SocketFactory, webSocketFactory } from './connection';
import { DEFAULT_TIMEOUT_MS, HttpRequest, HttpTransport, fetchTransport } from './http';
import {
  AppInfo,
  AuthMessage,
  CheckRequest,
  CheckTextRequest,
  ConfigText,
  DescribeRequest,
  CheckResult,
  ConfigList,
  Connections,
  ConnectionsRequest,
  DashboardResponse,
  Recording,
  SaveConfigRequest,
  SavedConfig,
  ScenarioList,
  SessionSummary,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readConfigText,
  readConnections,
  readDashboardResponse,
  readRecording,
  readScenarioList,
  readSavedConfig,
  readSessionAllowed,
  readSessionResponse,
} from './protocol';
import { ClientRun, RunControl } from './run';
import { ClientSession, SessionFeed } from './session';
import { ANSWER_TIMEOUT_MS, ClientTime, TimeControl } from './time';
import { webSocketUrl } from './urls';
import { VariableBinder } from './variable-binding';
import { ClientVariables, VariablesFeed } from './variables';
import { ErrorSink, Readable, Writable } from './values';

/**
 * Limits used while they are unknown (before the api/app answer). The same as the launcher's and server's defaults.
 * During a run, the ranges of step and speed are the launcher's (`allowed.step`, `allowed.speed` of its state;
 * `controls` of connect() carries them); the ones here are for choosing the speed of a run that has not started.
 */
export const DEFAULT_LIMITS: AppInfo['limits'] = {
  speed_min: 0.000001,
  speed_max: 1000000,
  step_ms_max: 1e12,
  check_timeout_s: 120,
  watch_period_ms_min: 20,
  watch_period_ms_max: 60_000,
  watches: 16,
  watch_variables: 2000,
  writes_per_request: 256,
  write_at_ms_max: 1e12,
};

/** Default onError: prints to stderr (only what subscribers threw; never the token). */
const reportToConsole: ErrorSink = (error) =>
  console.error('vehicle-sil gui-client: a subscriber threw (the client goes on):', error);

/** Options of createClient. */
export interface ClientOptions {
  /**
   * URL of the GUI server (`http://127.0.0.1:<port>/`: the URL the server printed, without `#token=...`).
   * HTTP paths and the WebSocket (`ws`) are relative to it. In the browser, use the server's root
   * (`new URL('/', location.href).href`). `document.baseURI` is the screen's route on a page without
   * `<base href="/">`.
   */
  baseUrl: string;
  /**
   * The token (what follows `#token=` in the URL the server printed). A function is called each time the token is
   * used. While it is null, the client does not connect. Switching to another token after a refusal does not
   * reconnect (create a new client instead).
   */
  token: string | (() => string | null);
  /**
   * Origin to send with POST and the WebSocket handshake. Not needed in browsers, which add it themselves. In Node,
   * the origin of baseUrl is sent if omitted (the server accepts only its own origin). Do not pass it in a Web Worker
   * (a Worker's WebSocket cannot add handshake headers; the browser adds Origin).
   */
  origin?: string;
  /** HTTP transport (default fetchTransport). */
  http?: HttpTransport;
  /** Function that opens the WebSocket (default webSocketFactory). */
  socket?: SocketFactory;
  /** How long to wait for the answer to a time command (ms; default ANSWER_TIMEOUT_MS). */
  answerTimeoutMs?: number;
  /**
   * Receives what subscribers (functions passed to subscribe, onBatch, onMessages, onUnreadable) threw. The client
   * carries on (other subscribers still get notified, and handling such as disconnecting after a refusal still
   * completes). The default prints to console.error.
   */
  onError?: (error: unknown) => void;
}

/**
 * Client for the GUI server. Holds the connection (WebSocket), the session copy, run and time control, and the server
 * information. All state comes as readable values (Readable: get and subscribe). No framework needed; runs in browsers
 * and in Node.
 *
 * Lifetime: connect() connects (calling it again while connected does nothing). close() disconnects, stops pending
 * timers, stops waiting for answers to time commands and fails writes still waiting for their result (variable
 * watches stay and resume on reconnect). connect() after close() connects again (the session copy is kept and
 * resumes from what it already has). HTTP requests (loadApp() and so on) work without a connection. After the server
 * refused the token (session.rejected) or the versions differ (session.incompatible), connect() does nothing (create a
 * new client).
 */
export interface GuiClient {
  /** The WebSocket connection (state, number of failed handshakes, close reason, notifications received). */
  readonly connection: ClientConnection;
  /** The session copy (current session, whether it is complete, notices). */
  readonly session: ClientSession;
  /** Run control (start, stop). */
  readonly run: ClientRun;
  /** Time state and commands. */
  readonly time: ClientTime;
  /** ECU variables: list and state, value watches, writes and their results. */
  readonly variables: ClientVariables;
  /** Server and launcher information (`GET api/app`, after loadApp()). null until then. */
  readonly app: Readable<AppInfo | null>;
  /** Speed range and the limits of stepping and checks (the limits of app; DEFAULT_LIMITS until then). */
  readonly limits: Readable<AppInfo['limits']>;
  /** Connects through WebSocket (does nothing without a token, after a refusal, or after a version mismatch). */
  connect(): void;
  /** Disconnects and stops pending timers. */
  close(): void;
  /** Reads `GET api/app` into app. */
  loadApp(): Promise<AppInfo>;
  /** `GET api/configs`: the configuration files to choose from. */
  configs(): Promise<ConfigList>;
  /**
   * `POST api/check`: checks a configuration file (a failed check still gets a successful answer; its result is then
   * not passed). Waits twice the server's check limit (limits.check_timeout_s; waiting its turn, then checking) plus
   * the default HTTP timeout. A check of a `text` may first describe the saved file in the same turn (the server does
   * it when it has no saved descriptions); the second limit covers that.
   */
  check(request: CheckRequest | CheckTextRequest): Promise<CheckResult>;
  /**
   * `GET api/config`: the text of a configuration file as it is saved, with the SHA-256 of its bytes (`stamp`).
   * Rejects with an HttpError (`not-a-configuration`, `config-too-large`, `config-unreadable`, ...).
   */
  config(file: string): Promise<ConfigText>;
  /**
   * `POST api/describe`: checks the saved file and saves its participants' descriptions again (after an executable
   * was built again), and returns that check. Waits in the queue of the checks, as long as `check`.
   */
  describe(request: DescribeRequest): Promise<CheckResult>;
  /**
   * `POST api/config`: saves a text as a configuration file, with the `stamp` it was read with (null: a new file).
   * Rejects with an HttpError (`save-disabled`, `commands-changed` (also a new file to a server started without
   * `--allow-command-changes`), `config-changed`, `config-busy`,
   * `config-unreadable`, `text-too-large`, ...). Waits in the queue of the checks, as long as `check`.
   */
  saveConfig(request: SaveConfigRequest): Promise<SavedConfig>;
  /**
   * `POST api/connections`: where one network of a configuration file can be connected on the computer of the GUI
   * server. It starts that network's bridge to list (on Windows, Npcap may ask to raise the rights), so ask only when
   * the user opens the choice. Waits in the queue of the checks, as long as `check`.
   */
  connections(request: ConnectionsRequest): Promise<Connections>;
  /** `GET api/session`: summary of the current session (null if none). Also applied to the session copy. */
  fetchSession(): Promise<SessionSummary | null>;
  /**
   * `GET api/dashboard`: reads the dashboard file next to the configuration file (relative to root) in the form the
   * server checked (found is false if there is none; problems are in errors).
   */
  dashboard(file: string): Promise<DashboardResponse>;
  /** `GET api/scenarios`: the scenario files in `scenarios` next to the configuration file (relative to root). */
  scenarios(file: string): Promise<ScenarioList>;
  /**
   * `GET api/recording`: the values written through this server and applied in the current (or last) run, as scenario
   * YAML (null if there has been no run yet). Not written to a file.
   */
  recording(): Promise<Recording | null>;
}

/**
 * How long to wait for the answer to a start (`api/run`) and a check (`api/check`) (ms). The server runs one check at
 * a time and spends up to check_timeout_s waiting and up to check_timeout_s checking (a start checks a changed
 * configuration file again). Twice that, plus the launcher's start-up and an HTTP margin (DEFAULT_TIMEOUT_MS).
 */
export function longRequestMs(limits: AppInfo['limits']): number {
  return 2 * limits.check_timeout_s * 1000 + DEFAULT_TIMEOUT_MS;
}

/** Whether this runs on Node (only Node's WebSocket can add handshake headers). */
export function isNode(runtime: object = globalThis): boolean {
  const process = (runtime as { process?: { versions?: { node?: unknown } } }).process;
  return typeof process?.versions?.node === 'string';
}

/** Default Origin outside the browser: the server's own origin on Node, none elsewhere. */
export function defaultOrigin(baseUrl: string, runtime: object = globalThis): string | undefined {
  return isNode(runtime) ? new URL(baseUrl).origin : undefined;
}

/** Creates a client for the GUI server. Creating it does not connect (call connect()). */
export function createClient(options: ClientOptions): GuiClient {
  return buildClient(options).client;
}

/** What the front (front.ts) needs besides the client: the transport and the error sink. */
export interface ClientParts {
  client: GuiClient;
  http: HttpTransport;
  onError: ErrorSink;
}

/** createClient, and the parts the front door is built from. */
export function buildClient(options: ClientOptions): ClientParts {
  const token = typeof options.token === 'function' ? options.token : () => options.token as string;
  const onError = options.onError ?? reportToConsole;
  const origin = options.origin ?? defaultOrigin(options.baseUrl);
  const http = options.http ?? fetchTransport({ baseUrl: options.baseUrl, token, origin });
  const app = new Writable<AppInfo | null>(null, Object.is, onError);
  const limits = new Writable<AppInfo['limits']>(DEFAULT_LIMITS, Object.is, onError);
  app.subscribe((info) => limits.set(info?.limits ?? DEFAULT_LIMITS));

  // Add each feature's fields to the authentication message (for the session copy: where to resend from)
  let feed: SessionFeed | null = null;
  let variablesFeed: VariablesFeed | null = null;
  const connection = new Connection({
    url: webSocketUrl(options.baseUrl, 'ws'),
    socket: options.socket ?? webSocketFactory({ origin }),
    greeting: (): AuthMessage[] => [
      {
        type: 'auth',
        token: token() ?? '',
        ...feed?.resumeFields(),
        session_without_check: true,
      },
    ],
    afterGreeting: () => variablesFeed?.resume() ?? [],
    onError,
  });
  feed = new SessionFeed(connection, onError);
  const session = feed;
  variablesFeed = new VariablesFeed(connection, http, onError, () => limits.get());
  const variables = variablesFeed;
  const run = new RunControl(session, http, onError, () => longRequestMs(limits.get()));
  const time = new TimeControl(
    session.session,
    http,
    options.answerTimeoutMs ?? ANSWER_TIMEOUT_MS,
    onError,
  );

  const binder = new VariableBinder({
    variables,
    connection: connection.state,
    session: session.session,
    onError,
  });

  /** Sends and parses. after is called with the parsed value before the Promise resolves (in the same call). */
  const call = <T>(req: HttpRequest, read: (body: unknown) => T, after?: (value: T) => void) =>
    new Promise<T>((resolve, reject) =>
      http(req, (outcome) => {
        if (!outcome.ok) {
          reject(outcome.error);
          return;
        }
        try {
          const value = read(outcome.body);
          after?.(value);
          resolve(value);
        } catch (error) {
          reject(error);
        }
      }),
    );

  // Expose only the read side and the safe operations (not the methods of the internal parts)
  const client: GuiClient = {
    connection: {
      state: connection.state,
      failures: connection.failures,
      lastClose: connection.lastClose,
      tryAgainReason: connection.tryAgainReason,
      onBatch: (listener) => connection.onBatch(listener),
      onUnreadable: (listener) => connection.onUnreadable(listener),
    },
    session: {
      session: session.session,
      currentTarget: () => session.currentTarget(),
      ready: session.ready,
      allowed: session.allowed,
      lastEventAt: session.lastEventAt,
      notices: session.notices,
      rejected: session.rejected,
      incompatible: session.incompatible,
      onMessages: (listener) => session.onMessages(listener),
      report: (message, count) => session.report(message, count),
      dismissNotice: (id) => session.dismissNotice(id),
    },
    run: {
      starting: run.starting,
      stopping: run.stopping,
      stopWhileStarting: run.stopWhileStarting,
      error: run.error,
      start: (request) => run.start(request),
      stop: () => run.stop(),
      forceStop: () => run.forceStop(),
    },
    time: {
      state: time.state,
      answerTimeoutMs: time.answerTimeoutMs,
      pause: () => time.pause(),
      resume: () => time.resume(),
      step: (ms) => time.step(ms),
      setSpeed: (value) => time.setSpeed(value),
      send: (command) => time.send(command),
    },
    variables: {
      state: variables.state,
      operations: variables.operations,
      watchProblem: variables.watchProblem,
      watch: (wanted, watchOptions) => variables.watch(wanted, watchOptions),
      write: (writes, writeOptions) => variables.write(writes, writeOptions),
      bind: (ref, bindOptions) => binder.bind(ref, bindOptions),
    },
    app: app.readonly(),
    limits: limits.readonly(),
    connect: () => {
      if (token() && !session.stopped) connection.connect();
    },
    close: () => {
      connection.close();
      time.dispose();
      variables.dispose();
    },
    loadApp: () => call({ method: 'GET', path: 'api/app' }, readAppInfo, (info) => app.set(info)),
    configs: () => call({ method: 'GET', path: 'api/configs' }, readConfigList),
    config: (file) =>
      call({ method: 'GET', path: `api/config?file=${encodeURIComponent(file)}` }, readConfigText),
    describe: (body) =>
      call(
        {
          method: 'POST',
          path: 'api/describe',
          body,
          timeoutMs: longRequestMs(limits.get()),
        },
        readCheckResponse,
      ),
    saveConfig: (body) =>
      call(
        { method: 'POST', path: 'api/config', body, timeoutMs: longRequestMs(limits.get()) },
        readSavedConfig,
      ),
    dashboard: (file) =>
      call(
        { method: 'GET', path: `api/dashboard?file=${encodeURIComponent(file)}` },
        readDashboardResponse,
      ),
    scenarios: (file) =>
      call(
        { method: 'GET', path: `api/scenarios?file=${encodeURIComponent(file)}` },
        readScenarioList,
      ),
    recording: () => call({ method: 'GET', path: 'api/recording' }, readRecording),
    check: (body) =>
      call(
        {
          method: 'POST',
          path: 'api/check',
          body,
          timeoutMs: longRequestMs(limits.get()),
        },
        readCheckResponse,
      ),
    connections: (body) =>
      call(
        {
          method: 'POST',
          path: 'api/connections',
          body,
          timeoutMs: longRequestMs(limits.get()),
        },
        readConnections,
      ),
    fetchSession: () => {
      const resyncFrom = session.currentInstance();
      return new Promise<SessionSummary | null>((resolve, reject) =>
        http({ method: 'GET', path: 'api/session' }, (outcome) => {
          if (!outcome.ok) {
            reject(outcome.error);
            return;
          }
          try {
            const summary = readSessionResponse(outcome.body);
            const instance = (outcome.body as { instance?: unknown } | null)?.instance;
            if (
              session.adopt(summary, typeof instance === 'string' ? instance : null, resyncFrom)
            ) {
              session.adoptAllowed(readSessionAllowed(outcome.body));
              // The variable list and state are not part of the summary parser (SessionSummary)
              variables.adoptSummary((outcome.body as { session?: unknown } | null)?.session);
            }
            resolve(summary);
          } catch (error) {
            reject(error);
          }
        }),
      );
    },
  };
  return { client, http, onError };
}
