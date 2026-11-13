import { ClientConnection, Connection, SocketFactory, webSocketFactory } from './connection';
import { DEFAULT_TIMEOUT_MS, HttpRequest, HttpTransport, fetchTransport } from './http';
import {
  AppInfo,
  AuthMessage,
  CheckRequest,
  CheckResult,
  ConfigList,
  DashboardResponse,
  Recording,
  ScenarioList,
  SessionSummary,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readDashboardResponse,
  readRecording,
  readScenarioList,
  readSessionResponse,
} from './protocol';
import { ClientRun, RunControl } from './run';
import { ClientSession, SessionFeed } from './session';
import { ANSWER_TIMEOUT_MS, ClientTime, TimeControl } from './time';
import { webSocketUrl } from './urls';
import { ClientVariables, VariablesFeed } from './variables';
import { ErrorSink, Readable, Writable } from './values';

/** 上限を知らないとき（api/app の答えの前）の値。ランチャーとサーバーの既定と同じ。 */
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

/** 既定の onError: 標準エラーに出す（購読者が投げたものだけ。トークンは入れない）。 */
const reportToConsole: ErrorSink = (error) =>
  console.error('vehicle-sil gui-client: a subscriber threw (the client goes on):', error);

/** createClient の設定。 */
export interface ClientOptions {
  /**
   * GUI サーバーの URL（`http://127.0.0.1:<port>/`。サーバーが表示した URL から `#token=...` を除いたもの）。
   * HTTP の path と WebSocket（`ws`）は、ここからの相対。ブラウザでは、サーバーの根（`new URL('/', location.href).href`）。`document.baseURI` は、`<base href="/">` のないページでは画面の経路になる。
   */
  baseUrl: string;
  /**
   * トークン（サーバーが表示した URL の `#token=` の後ろ）。関数なら、使うたびに呼ぶ。null の間はつながない。
   * 断られた後に別のトークンに替えても、つなぎ直さない（クライアントを作り直す）。
   */
  token: string | (() => string | null);
  /**
   * POST と WebSocket の握手に付ける Origin。ブラウザは自分で付けるので要らない。Node では、省くと baseUrl の
   * origin を付ける（サーバーは、自分自身の origin だけを受け付ける）。Web Worker では渡さない（Worker の
   * WebSocket は握手のヘッダーを付けられない。Origin はブラウザが付ける）。
   */
  origin?: string;
  /** HTTP の口（既定は fetchTransport）。 */
  http?: HttpTransport;
  /** WebSocket を開く関数（既定は webSocketFactory）。 */
  socket?: SocketFactory;
  /** 時刻の命令の答えを待つ時間（ミリ秒。既定は ANSWER_TIMEOUT_MS）。 */
  answerTimeoutMs?: number;
  /**
   * 購読者（subscribe、onBatch、onMessages、onUnreadable に渡した関数）が投げた例外を受ける。クライアントは
   * 止まらずに進む（ほかの購読者にも届き、断られたら切る、などの処理も済む）。既定は console.error に出す。
   */
  onError?: (error: unknown) => void;
}

/**
 * GUI サーバーのクライアント。接続（WebSocket）、セッションの写し、実行と時刻の操作、サーバーの情報を持つ。
 * 状態はどれも読める値（Readable: get と subscribe）で出す。枠組みに依らず、ブラウザでも Node でも動く。
 *
 * 寿命: connect() でつなぐ（つながっている間にもう一度呼んでも何もしない）。close() で切り、待っている時計を止め、
 * 答えを待っている時刻の命令を待つのをやめ、結果を待っている書き込みを断る（変数の watch は残り、つなぎ直すと続く）。close() の後に connect() すると、またつなぐ（セッションの写しは
 * 残り、持っている分の続きから受け直す）。HTTP の要求（loadApp() など）は、つないでいなくても使える。サーバーが
 * トークンを断った（session.rejected）か、版が違う（session.incompatible）後の connect() は何もしない
 * （クライアントを作り直す）。
 */
export interface GuiClient {
  /** WebSocket の接続（状態、握手の失敗の数、閉じた理由、届いた通知）。 */
  readonly connection: ClientConnection;
  /** セッションの写し（いまのセッション、揃ったか、知らせ）。 */
  readonly session: ClientSession;
  /** 実行の操作（開始、停止）。 */
  readonly run: ClientRun;
  /** 時刻の状態と命令。 */
  readonly time: ClientTime;
  /** ECU の変数: 一覧と状態、値の watch、書き込みと結果。 */
  readonly variables: ClientVariables;
  /** サーバーとランチャーの情報（`GET api/app`。loadApp() の後）。まだなければ null。 */
  readonly app: Readable<AppInfo | null>;
  /** 速さの範囲、ステップ送りと照合の上限（app の limits。まだなければ DEFAULT_LIMITS）。 */
  readonly limits: Readable<AppInfo['limits']>;
  /** WebSocket でつなぐ（トークンがないとき、断られた後、版が違った後は何もしない）。 */
  connect(): void;
  /** 切って、待っている時計を止める。 */
  close(): void;
  /** `GET api/app` を読み、app に入れる。 */
  loadApp(): Promise<AppInfo>;
  /** `GET api/configs`: 選べる構成ファイル。 */
  configs(): Promise<ConfigList>;
  /**
   * `POST api/check`: 構成ファイルを照合する（照合が失敗しても成功の応答で、結果の result が passed でない）。
   * サーバーの照合の上限（limits.check_timeout_s）の 2 倍（順番待ちと照合）に、HTTP の既定の待ち時間を足して待つ。
   */
  check(request: CheckRequest): Promise<CheckResult>;
  /** `GET api/session`: いまのセッションの要約（なければ null）。写しにも当てる。 */
  fetchSession(): Promise<SessionSummary | null>;
  /**
   * `GET api/dashboard`: 構成ファイル（root からの相対）の隣のダッシュボードのファイルを、サーバーが確かめた形で
   * 読む（ないときは found が偽、誤りは errors）。
   */
  dashboard(file: string): Promise<DashboardResponse>;
  /** `GET api/scenarios`: 構成ファイル（root からの相対）の隣の `scenarios` にあるシナリオのファイル。 */
  scenarios(file: string): Promise<ScenarioList>;
  /**
   * `GET api/recording`: いまの（または最後の）実行で、このサーバーを通して書いて反映された値を、シナリオの YAML に
   * したもの（実行がまだなければ null）。ファイルには書かない。
   */
  recording(): Promise<Recording | null>;
}

/**
 * 開始（`api/run`）と照合（`api/check`）の応答を待つ時間（ミリ秒）。サーバーは照合を 1 つずつ行い、待つのに
 * check_timeout_s、照合に check_timeout_s までかける（開始は、変わった構成ファイルを照合し直す）。その 2 倍に、
 * ランチャーの起動と HTTP の余裕（DEFAULT_TIMEOUT_MS）を足す。
 */
export function longRequestMs(limits: AppInfo['limits']): number {
  return 2 * limits.check_timeout_s * 1000 + DEFAULT_TIMEOUT_MS;
}

/** 動いているのが Node か（Node の WebSocket だけが握手のヘッダーを付けられる）。 */
export function isNode(runtime: object = globalThis): boolean {
  const process = (runtime as { process?: { versions?: { node?: unknown } } }).process;
  return typeof process?.versions?.node === 'string';
}

/** ブラウザの外で既定に付ける Origin: Node ならサーバー自身の origin、ほかでは付けない。 */
export function defaultOrigin(baseUrl: string, runtime: object = globalThis): string | undefined {
  return isNode(runtime) ? new URL(baseUrl).origin : undefined;
}

/** GUI サーバーのクライアントを作る。作っただけではつながない（connect()）。 */
export function createClient(options: ClientOptions): GuiClient {
  const token = typeof options.token === 'function' ? options.token : () => options.token as string;
  const onError = options.onError ?? reportToConsole;
  const origin = options.origin ?? defaultOrigin(options.baseUrl);
  const http = options.http ?? fetchTransport({ baseUrl: options.baseUrl, token, origin });
  const app = new Writable<AppInfo | null>(null, Object.is, onError);
  const limits = new Writable<AppInfo['limits']>(DEFAULT_LIMITS, Object.is, onError);
  app.subscribe((info) => limits.set(info?.limits ?? DEFAULT_LIMITS));

  // 認証のメッセージに、機能ごとの欄を足す（セッションの写しは、どこから再送してほしいか）
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
    session.ready,
    app.readonly(),
    http,
    options.answerTimeoutMs ?? ANSWER_TIMEOUT_MS,
    onError,
    variables.state,
  );

  /** 送って読む。after は、読めた値で Promise が解ける前に（同じ呼び出しの中で）呼ぶ。 */
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

  // 外へは、読むための口と、壊さない操作だけを出す（中の部品のメソッドは見せない）
  return {
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
      ready: session.ready,
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
    fetchSession: () =>
      new Promise<SessionSummary | null>((resolve, reject) =>
        http({ method: 'GET', path: 'api/session' }, (outcome) => {
          if (!outcome.ok) {
            reject(outcome.error);
            return;
          }
          try {
            const summary = readSessionResponse(outcome.body);
            session.adopt(summary);
            // 変数の一覧と状態は、要約の読み取り（SessionSummary）の外にある
            variables.adoptSummary((outcome.body as { session?: unknown } | null)?.session);
            resolve(summary);
          } catch (error) {
            reject(error);
          }
        }),
      ),
  };
}
