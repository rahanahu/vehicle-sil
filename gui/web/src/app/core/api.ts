import { HttpErrorResponse } from '@angular/common/http';

/**
 * サーバーとの間でやりとりするデータの形と、その読み取り関数（正本は gui/protocol/）。
 *
 * サーバーとの間の型はここにだけ書き、画面はここから import する。
 * 届いた JSON は、ここの read* 関数で型を確かめてから使う（httpResource の parse、WebSocket の通知）。
 * - 知らない欄は無視する。知らない事象と知らない通知は null にして読み飛ばす。
 * - 値の集合が増えうる欄（段階、状態、結果など）は、知らない値も文字列のまま通す。
 * - 形が違うもの（欄がない、型が違う、知らない形式の版）は ProtocolError を投げる。
 * 単体試験（api.spec.ts）が gui/protocol/examples/ の実例をすべて読ませて確かめる。
 */

/** 読めない形のデータ。path は壊れていた場所（例: `event.participants[0].name`）。 */
export class ProtocolError extends Error {
  constructor(
    readonly path: string,
    readonly expected: string,
  ) {
    super(`${path}: expected ${expected}`);
    this.name = 'ProtocolError';
  }
}

/** 値の集合が増えうる文字列。知っている値は補完が効き、知らない値も入る。 */
export type Open<T extends string> = T | (string & {});

// --- 読み取りの部品 ---------------------------------------------------------------------------

type Reader<T> = (value: unknown, path: string) => T;
type Fields = Record<string, unknown>;

function object(value: unknown, path: string): Fields {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new ProtocolError(path, 'an object');
  }
  return value as Fields;
}

const string: Reader<string> = (value, path) => {
  if (typeof value !== 'string') throw new ProtocolError(path, 'a string');
  return value;
};

const number: Reader<number> = (value, path) => {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new ProtocolError(path, 'a number');
  }
  return value;
};

const integer: Reader<number> = (value, path) => {
  if (!Number.isInteger(value)) throw new ProtocolError(path, 'an integer');
  return value as number;
};

const boolean: Reader<boolean> = (value, path) => {
  if (typeof value !== 'boolean') throw new ProtocolError(path, 'true or false');
  return value;
};

function nullable<T>(read: Reader<T>): Reader<T | null> {
  return (value, path) => (value === null ? null : read(value, path));
}

/** 欄がない（undefined）ときは fallback。古いランチャーがまだ出さない欄に使う。 */
function optional<T, F>(read: Reader<T>, fallback: F): Reader<T | F> {
  return (value, path) => (value === undefined ? fallback : read(value, path));
}

function array<T>(read: Reader<T>): Reader<T[]> {
  return (value, path) => {
    if (!Array.isArray(value)) throw new ProtocolError(path, 'an array');
    return value.map((item, i) => read(item, `${path}[${i}]`));
  };
}

function field<T>(fields: Fields, path: string, key: string, read: Reader<T>): T {
  return read(fields[key], `${path}.${key}`);
}

/** 形式の版の欄。知らない版は読まない（意味が変わっているかもしれないため）。 */
function version(fields: Fields, path: string, key: string, expected: number): void {
  if (fields[key] !== expected) {
    throw new ProtocolError(`${path}.${key}`, `format version ${expected}`);
  }
}

// --- ランチャー: 照合（check --json と同じ形） --------------------------------------------------

/** 所見の重さ。知らない重さは誤りとして扱う（severityRank）。 */
export type Severity = Open<'error' | 'warning' | 'note'>;

/** 照合と実行の所見。message と detail は英語のまま出す。 */
export interface Finding {
  severity: Severity;
  code: string;
  file: string | null;
  line: number | null;
  participant: string | null;
  network: string | null;
  message: string;
  detail: string | null;
}

/** 重い順に並べるための順位（error 0、warning 1、note 2。知らない重さは error と同じ）。 */
export function severityRank(severity: Severity): number {
  return severity === 'note' ? 2 : severity === 'warning' ? 1 : 0;
}

export const readFinding: Reader<Finding> = (value, path) => {
  const f = object(value, path);
  return {
    severity: field(f, path, 'severity', string),
    code: field(f, path, 'code', string),
    file: field(f, path, 'file', nullable(string)),
    line: field(f, path, 'line', nullable(integer)),
    participant: field(f, path, 'participant', nullable(string)),
    network: field(f, path, 'network', nullable(string)),
    message: field(f, path, 'message', string),
    detail: field(f, path, 'detail', nullable(string)),
  };
};

/** ECU が自己申告した変数（実行前にパネルを描くため）。 */
export interface Variable {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  causality: Open<'input' | 'output' | 'parameter'>;
  unit: string;
  min: number | null;
  max: number | null;
}

const readVariable: Reader<Variable> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', string),
    causality: field(f, path, 'causality', string),
    unit: field(f, path, 'unit', optional(string, '')),
    min: field(f, path, 'min', optional(nullable(number), null)),
    max: field(f, path, 'max', optional(nullable(number), null)),
  };
};

export type CheckStatus = Open<'described' | 'not_checked' | 'failed' | 'skipped' | 'disabled'>;

export interface CheckParticipant {
  /** 読めなかったときは null。 */
  name: string | null;
  line: number | null;
  required: boolean | null;
  enabled: boolean | null;
  step_ms: number | null;
  status: CheckStatus;
  /** 自己申告の変数。null は「わからない」（申告がない、外した、読めなかった）、空の配列は「0 個」。 */
  variables: Variable[] | null;
}

const readCheckParticipant: Reader<CheckParticipant> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', nullable(string)),
    line: field(f, path, 'line', optional(nullable(integer), null)),
    required: field(f, path, 'required', nullable(boolean)),
    enabled: field(f, path, 'enabled', nullable(boolean)),
    step_ms: field(f, path, 'step_ms', nullable(number)),
    status: field(f, path, 'status', string),
    variables: field(f, path, 'variables', optional(nullable(array(readVariable)), null)),
  };
};

export interface CheckNetwork {
  name: string | null;
  kind: string | null;
  /** 使う participant 名（構成ファイルの順）。 */
  participants: string[];
}

const readCheckNetwork: Reader<CheckNetwork> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', nullable(string)),
    kind: field(f, path, 'kind', nullable(string)),
    participants: field(f, path, 'participants', array(string)),
  };
};

export type CheckOutcome = Open<'passed' | 'failed' | 'unreadable' | 'usage'>;

/** `vehicle_sil_launch check --json` の結果（形式の版 1）。 */
export interface CheckResult {
  file: string | null;
  result: CheckOutcome;
  errors: number;
  warnings: number;
  notes: number;
  stop_at_ms: number | null;
  participants: CheckParticipant[];
  networks: CheckNetwork[];
  findings: Finding[];
}

export const readCheckResult: Reader<CheckResult> = (value, path) => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_check', 1);
  return {
    file: field(f, path, 'file', nullable(string)),
    result: field(f, path, 'result', string),
    errors: field(f, path, 'errors', integer),
    warnings: field(f, path, 'warnings', integer),
    notes: field(f, path, 'notes', integer),
    stop_at_ms: field(f, path, 'stop_at_ms', nullable(number)),
    participants: field(f, path, 'participants', array(readCheckParticipant)),
    networks: field(f, path, 'networks', array(readCheckNetwork)),
    findings: field(f, path, 'findings', array(readFinding)),
  };
};

// --- ランチャー: 実行の結果（run --json と同じ形） ------------------------------------------------

export interface RunParticipant {
  name: string;
  required: boolean;
  enabled: boolean;
  ended: Open<'exited' | 'stopped' | 'killed' | 'not_started'>;
  exit_code: number | null;
  signal: string | null;
  left_processes: boolean;
}

const readRunParticipant: Reader<RunParticipant> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    required: field(f, path, 'required', boolean),
    enabled: field(f, path, 'enabled', boolean),
    ended: field(f, path, 'ended', string),
    exit_code: field(f, path, 'exit_code', nullable(integer)),
    signal: field(f, path, 'signal', nullable(string)),
    left_processes: field(f, path, 'left_processes', boolean),
  };
};

export type RunOutcome = Open<
  'passed' | 'failed' | 'interrupted' | 'check_failed' | 'unreadable' | 'setup_failed' | 'usage'
>;

/** `vehicle_sil_launch run --json` の結果（形式の版 1）。 */
export interface RunResult {
  file: string | null;
  result: RunOutcome;
  errors: number;
  warnings: number;
  notes: number;
  simulation: string | null;
  interrupted_by: string | null;
  participants: RunParticipant[];
  findings: Finding[];
  check: CheckResult | null;
}

export const readRunResult: Reader<RunResult> = (value, path) => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_run', 1);
  return {
    file: field(f, path, 'file', nullable(string)),
    result: field(f, path, 'result', string),
    errors: field(f, path, 'errors', integer),
    warnings: field(f, path, 'warnings', integer),
    notes: field(f, path, 'notes', integer),
    simulation: field(f, path, 'simulation', nullable(string)),
    interrupted_by: field(f, path, 'interrupted_by', nullable(string)),
    participants: field(f, path, 'participants', array(readRunParticipant)),
    findings: field(f, path, 'findings', array(readFinding)),
    check: field(f, path, 'check', optional(nullable(readCheckResult), null)),
  };
};

// --- ランチャー: 経過の事象（run --events の 1 行） ---------------------------------------------

/** 段階。この順に進み、飛ばすことはあるが戻らない。 */
export const PHASES = ['checking', 'starting', 'waiting', 'running', 'stopping', 'ended'] as const;
export type Phase = Open<(typeof PHASES)[number]>;

export type ParticipantState = Open<
  'disabled' | 'started' | 'joined' | 'ended' | 'stop_requested' | 'killed'
>;

interface EventBase {
  seq: number;
  /** ランチャーの開始からの実時間（ミリ秒）。 */
  t_ms: number;
}

export type LauncherEvent =
  | (EventBase & {
      event: 'hello';
      vehicle_sil_events: number;
      vehicle_sil: string;
      pid: number | null;
      file: string | null;
      arguments: string[];
    })
  | (EventBase & { event: 'phase'; phase: Phase })
  | (EventBase & { event: 'check'; check: CheckResult })
  | (EventBase & { event: 'registry'; uri: string })
  | (EventBase & {
      event: 'participant';
      name: string;
      state: ParticipantState;
      pid: number | null;
      exit_code: number | null;
      signal: string | null;
    })
  | (EventBase & { event: 'log'; source: string; line: string })
  | (EventBase & { event: 'finding'; finding: Finding })
  | (EventBase & { event: 'time'; sim_ms: number })
  | (EventBase & { event: 'command'; command: string; accepted: boolean; message: string | null })
  | (EventBase & { event: 'result'; result: RunResult });

/** ランチャーの事象を 1 つ読む。知らない事象は null（読み飛ばす）。 */
export const readLauncherEvent: Reader<LauncherEvent | null> = (value, path) => {
  const f = object(value, path);
  const base: EventBase = {
    seq: field(f, path, 'seq', integer),
    t_ms: field(f, path, 't_ms', number),
  };
  const kind = field(f, path, 'event', string);
  switch (kind) {
    case 'hello':
      // 形式の版の食い違いはサーバーが見つけて知らせる。画面は読める欄だけを使う
      return {
        ...base,
        event: kind,
        vehicle_sil_events: field(f, path, 'vehicle_sil_events', integer),
        vehicle_sil: field(f, path, 'vehicle_sil', string),
        pid: field(f, path, 'pid', optional(nullable(integer), null)),
        file: field(f, path, 'file', optional(nullable(string), null)),
        arguments: field(f, path, 'arguments', optional(array(string), [])),
      };
    case 'phase':
      return { ...base, event: kind, phase: field(f, path, 'phase', string) };
    case 'check':
      return { ...base, event: kind, check: field(f, path, 'check', readCheckResult) };
    case 'registry':
      return { ...base, event: kind, uri: field(f, path, 'uri', string) };
    case 'participant':
      return {
        ...base,
        event: kind,
        name: field(f, path, 'name', string),
        state: field(f, path, 'state', string),
        pid: field(f, path, 'pid', optional(nullable(integer), null)),
        exit_code: field(f, path, 'exit_code', optional(nullable(integer), null)),
        signal: field(f, path, 'signal', optional(nullable(string), null)),
      };
    case 'log':
      return {
        ...base,
        event: kind,
        source: field(f, path, 'source', string),
        line: field(f, path, 'line', string),
      };
    case 'finding':
      // 所見の欄は事象の行にそのまま並ぶ
      return { ...base, event: kind, finding: readFinding(f, path) };
    case 'time':
      return { ...base, event: kind, sim_ms: field(f, path, 'sim_ms', number) };
    case 'command':
      return {
        ...base,
        event: kind,
        command: field(f, path, 'command', string),
        accepted: field(f, path, 'accepted', boolean),
        message: field(f, path, 'message', optional(nullable(string), null)),
      };
    case 'result':
      return { ...base, event: kind, result: field(f, path, 'result', readRunResult) };
    default:
      return null;
  }
};

// --- GUI サーバー: 共通 -------------------------------------------------------------------------

/** サーバーの知らせ（API の誤り、WebSocket の error、セッションの problems）。message は英語のまま出す。 */
export interface ServerProblem {
  code: string;
  message: string;
  /** launcher-stderr の、ここで落とした行の数（`detail.skipped`）。ほかの知らせにはない。 */
  skipped?: number;
}

const readProblem: Reader<ServerProblem> = (value, path) => {
  const f = object(value, path);
  const problem: ServerProblem = {
    code: field(f, path, 'code', string),
    message: field(f, path, 'message', string),
  };
  // detail の中身は code ごとに違う。使うものだけを、形が合うときだけ取る
  const detail = f['detail'];
  if (typeof detail === 'object' && detail !== null && 'skipped' in detail) {
    const skipped = detail.skipped;
    if (typeof skipped === 'number' && Number.isSafeInteger(skipped) && skipped >= 0) {
      problem.skipped = skipped;
    }
  }
  return problem;
};

/**
 * HTTP の失敗から、画面に出す文を取り出す。サーバーの誤りの形（`{"error": {"code", "message", "detail"}}`）なら
 * その message、ほかは状態の番号。
 */
export function errorMessage(error: unknown): string {
  if (error instanceof HttpErrorResponse) {
    try {
      return readProblem(object(error.error, 'response')['error'], 'response.error').message;
    } catch {
      return `HTTP ${error.status} ${error.statusText}`.trim();
    }
  }
  return error instanceof Error ? error.message : String(error);
}

/** 速度。null はランチャーの既定（構成ファイルの値）、'max' は最速、数は倍率（1 が等速）。 */
export type Speed = null | number | 'max';

const readSpeed: Reader<Speed> = (value, path) =>
  value === null || value === 'max' ? value : number(value, path);

// --- GUI サーバー: HTTP の API -----------------------------------------------------------------

/** GET api/app: サーバーとランチャーの版、使えるか、上限。 */
export interface AppInfo {
  version: string;
  /** サーバーの起動ごとに変わる識別子。変わったら、サーバーが起動し直した。 */
  instance: string;
  /** 構成ファイルを選べる範囲（表示用）。 */
  root: string;
  /** 起動のときに渡された構成ファイル（root からの相対）。なければ null。 */
  initial_file: string | null;
  launcher: {
    path: string | null;
    /** 画面に要る機能をランチャーが持っているか。 */
    usable: boolean;
    /** 使えない理由（英語のまま出す）。 */
    problems: ServerProblem[];
    features: string[];
  };
  limits: {
    speed_min: number;
    speed_max: number;
  };
}

export const readAppInfo = (value: unknown): AppInfo => {
  const path = 'app';
  const f = object(value, path);
  version(f, path, 'vehicle_sil_gui', 1);
  const lp = `${path}.launcher`;
  const launcher = field(f, path, 'launcher', object);
  const launcherVersion = field(launcher, lp, 'version', nullable(object));
  const limits = field(f, path, 'limits', object);
  return {
    version: field(f, path, 'version', string),
    instance: field(f, path, 'instance', string),
    root: field(f, path, 'root', string),
    initial_file: field(f, path, 'initial_file', nullable(string)),
    launcher: {
      path: field(launcher, lp, 'path', nullable(string)),
      usable: field(launcher, lp, 'usable', boolean),
      problems: field(launcher, lp, 'problems', array(readProblem)),
      features: launcherVersion
        ? field(launcherVersion, `${lp}.version`, 'features', optional(array(string), []))
        : [],
    },
    limits: {
      speed_min: field(limits, `${path}.limits`, 'speed_min', number),
      speed_max: field(limits, `${path}.limits`, 'speed_max', number),
    },
  };
};

/** GET api/configs の 1 件。 */
export interface ConfigFile {
  /** root からの相対パス（区切りは /）。 */
  path: string;
}

/** GET api/configs: root の下の構成ファイル。 */
export interface ConfigList {
  files: ConfigFile[];
  /** 上限で打ち切ったか。 */
  truncated: boolean;
}

export const readConfigList = (value: unknown): ConfigList => {
  const path = 'configs';
  const f = object(value, path);
  return {
    files: field(
      f,
      path,
      'files',
      array((item, p) => ({ path: field(object(item, p), p, 'path', string) })),
    ),
    truncated: field(f, path, 'truncated', boolean),
  };
};

/** POST api/check の本文。 */
export interface CheckRequest {
  /** root からの相対パス。 */
  file: string;
  /** 外す participant の名前（照合の結果にある名前だけ）。 */
  disable: string[];
}

/** POST api/check の応答から、ランチャーの照合の結果を取り出す。 */
export const readCheckResponse = (value: unknown): CheckResult =>
  field(object(value, 'response'), 'response', 'check', readCheckResult);

/** POST api/run の本文。 */
export interface RunRequest extends CheckRequest {
  speed: Speed;
  start_paused: boolean;
}

/** セッションの要約の participant（participant の事象を名前ごとに重ねたもの。まだ届いていない欄は null）。 */
export interface SummaryParticipant {
  name: string;
  state: ParticipantState | null;
  pid: number | null;
  exit_code: number | null;
  signal: string | null;
  /** 大きすぎて要約に入れなかった欄の名前（後の事象でその欄が届けば消える）。 */
  omitted_fields: string[];
}

const readSummaryParticipant: Reader<SummaryParticipant> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    state: field(f, path, 'state', optional(nullable(string), null)),
    pid: field(f, path, 'pid', optional(nullable(integer), null)),
    exit_code: field(f, path, 'exit_code', optional(nullable(integer), null)),
    signal: field(f, path, 'signal', optional(nullable(string), null)),
    omitted_fields: field(f, path, 'omitted_fields', optional(array(string), [])),
  };
};

/** 要約の launcher: ランチャーのプロセスが動いているか、終わったならその終わり方。 */
export interface SummaryLauncher {
  /** 動いているか。結果を出した後も、プロセスが終わるまでは true。止められるかはこれで決める。 */
  running: boolean;
  exit_code: number | null;
  /** 終わらせたシグナルの番号（POSIX だけ）。 */
  signal: number | null;
}

const readSummaryLauncher: Reader<SummaryLauncher> = (value, path) => {
  const f = object(value, path);
  return {
    running: field(f, path, 'running', boolean),
    exit_code: field(f, path, 'exit_code', nullable(integer)),
    signal: field(f, path, 'signal', nullable(integer)),
  };
};

/** サーバーがランチャーに頼んだ停止。none: 頼んでいない、requested: 順に止める、forced: 待たずに止める。 */
export type StopRequest = Open<'none' | 'requested' | 'forced'>;

/** 出どころごとの、サーバーが覚えている行と落とした行の数。 */
export interface LogCounts {
  kept: number;
  dropped: number;
}

const readLogCounts: Reader<LogCounts> = (value, path) => {
  const f = object(value, path);
  return { kept: field(f, path, 'kept', integer), dropped: field(f, path, 'dropped', integer) };
};

function record<T>(read: Reader<T>): Reader<Record<string, T>> {
  return (value, path) =>
    Object.fromEntries(
      Object.entries(object(value, path)).map(([key, item]) => [key, read(item, `${path}.${key}`)]),
    );
}

/** サーバーのセッション（ランチャーの 1 回の run）の要約。画面の状態はこれで作り直す（要約が正）。 */
export interface SessionSummary {
  id: number;
  /** root からの相対パス。 */
  file: string;
  disable: string[];
  speed: Speed;
  /** サーバーから見た状態。`ended` なら、ランチャーは終わり、その出力も読み終えた。 */
  state: Open<'running' | 'ended'>;
  launcher: SummaryLauncher;
  stop: StopRequest;
  phase: Phase | null;
  last_seq: number | null;
  /** 最後に受け取った事象の t_ms。 */
  last_t_ms: number | null;
  /** 最後の time 事象の仮想時刻。 */
  sim_ms: number | null;
  /** 最後の time 事象の、大きすぎて要約に入れなかった欄の名前。 */
  time_omitted_fields: string[];
  /** この run の照合の結果（check の事象）。まだなければ null。 */
  check: CheckResult | null;
  participants: SummaryParticipant[];
  findings: Finding[];
  result: RunResult | null;
  /** 出どころごとの、サーバーが覚えている行と落とした行の数。 */
  log: Record<string, LogCounts>;
  problems: ServerProblem[];
  problems_omitted: number;
}

export const readSessionSummary: Reader<SessionSummary> = (value, path) => {
  const f = object(value, path);
  const time = field(f, path, 'time', nullable(object));
  return {
    id: field(f, path, 'id', integer),
    file: field(f, path, 'file', string),
    disable: field(f, path, 'disable', array(string)),
    speed: field(f, path, 'speed', readSpeed),
    state: field(f, path, 'state', string),
    launcher: field(f, path, 'launcher', readSummaryLauncher),
    stop: field(f, path, 'stop', string),
    phase: field(f, path, 'phase', nullable(string)),
    last_seq: field(f, path, 'last_seq', nullable(integer)),
    last_t_ms: field(f, path, 'last_t_ms', nullable(number)),
    sim_ms: time ? field(time, `${path}.time`, 'sim_ms', number) : null,
    time_omitted_fields: time
      ? field(time, `${path}.time`, 'omitted_fields', optional(array(string), []))
      : [],
    check: field(f, path, 'check', nullable(readCheckResult)),
    participants: field(f, path, 'participants', array(readSummaryParticipant)),
    findings: field(f, path, 'findings', array(readFinding)),
    result: field(f, path, 'result', nullable(readRunResult)),
    log: field(f, path, 'log', record(readLogCounts)),
    problems: field(f, path, 'problems', array(readProblem)),
    problems_omitted: field(f, path, 'problems_omitted', integer),
  };
};

/** GET api/session、POST api/run、POST api/stop の応答（いまのセッション。なければ null）。 */
export const readSessionResponse = (value: unknown): SessionSummary | null =>
  field(object(value, 'response'), 'response', 'session', nullable(readSessionSummary));

/** POST api/stop の本文。force: false は順に止める（何度頼んでも同じ）、true は待たずに止める。 */
export interface StopRequestBody {
  force: boolean;
}

// --- GUI サーバー: WebSocket -------------------------------------------------------------------

/** つないだ直後に送る最初のメッセージ。session と from_seq を言うと、その続きから再送される。 */
export interface AuthMessage {
  type: 'auth';
  token: string;
  session?: number;
  from_seq?: number;
}

/** hello の replay: どこから再送するか（null は最初から）。 */
export interface Replay {
  from_seq: number | null;
}

const readReplay: Reader<Replay> = (value, path) => {
  const f = object(value, path);
  return { from_seq: field(f, path, 'from_seq', nullable(integer)) };
};

/** dropped: この画面に送らなかったものの数。 */
export interface Dropped {
  type: 'dropped';
  session: number | null;
  /** not-kept: サーバーが覚えていない（再送の前）、slow-client: 画面の受け取りが遅く、送る分から落とした。 */
  reason: Open<'not-kept' | 'slow-client'>;
  /** 出どころごとの、落とした log の行の数。 */
  log: Record<string, number>;
  /** 数が下限でしかない（もっと落ちているかもしれない）出どころ。 */
  at_least: string[];
  /** 落としたほかの事象の数。 */
  events: number;
}

export type ServerMessage =
  /** つないだ直後（と resync の後）。いまのセッションの要約と、続く再送の説明。 */
  | { type: 'hello'; instance: string; session: SessionSummary | null; replay: Replay | null }
  /** ランチャーの事象（再送の log の行も、新しいものも）。 */
  | { type: 'event'; session: number; event: LauncherEvent }
  /** 再送が終わった。 */
  | { type: 'synced'; session: number | null; last_seq: number | null }
  /** セッションの要約が変わった（始まった、停止を頼んだ、ランチャーが終わった、実行が終わった）。 */
  | { type: 'session'; session: SessionSummary }
  /** サーバーの知らせ（seq の抜け、ランチャーの不正な行、結果のない終わり、認証の失敗など）。 */
  | ({ type: 'error'; session: number | null } & ServerProblem)
  /** 送らなかったものの数。 */
  | Dropped
  /** 画面が読むのが遅く、メッセージを落とした。続けて hello から送り直される。 */
  | { type: 'resync'; message: string };

/** WebSocket の通知を 1 つ読む。知らない通知と、知らない事象を包んだ通知は null（読み飛ばす）。 */
export const readServerMessage = (value: unknown): ServerMessage | null => {
  const path = 'message';
  const f = object(value, path);
  const type = field(f, path, 'type', string);
  switch (type) {
    case 'hello':
      version(f, path, 'vehicle_sil_gui', 1);
      return {
        type,
        instance: field(f, path, 'instance', string),
        session: field(f, path, 'session', nullable(readSessionSummary)),
        replay: field(f, path, 'replay', nullable(readReplay)),
      };
    case 'event': {
      const event = field(f, path, 'event', readLauncherEvent);
      return event && { type, session: field(f, path, 'session', integer), event };
    }
    case 'synced':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        last_seq: field(f, path, 'last_seq', nullable(integer)),
      };
    case 'session':
      return { type, session: field(f, path, 'session', readSessionSummary) };
    case 'error':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        ...readProblem(value, path),
      };
    case 'dropped':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        reason: field(f, path, 'reason', string),
        log: field(f, path, 'log', record(integer)),
        at_least: field(f, path, 'at_least', array(string)),
        events: field(f, path, 'events', integer),
      };
    case 'resync':
      return { type, message: field(f, path, 'message', optional(string, '')) };
    default:
      return null;
  }
};
