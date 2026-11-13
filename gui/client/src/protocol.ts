/**
 * サーバーとの間でやりとりするデータの形と、その読み取り関数（正本は gui/protocol/）。
 *
 * サーバーとの間の型はここにだけ書き、ライブラリの利用者（画面も）は入口（index.ts）から import する。
 * 届いた JSON は、ここの read* 関数で型を確かめてから使う（HTTP の応答、WebSocket の通知）。
 * - 知らない欄は無視する。知らない事象と知らない通知は null にして読み飛ばす。
 * - 値の集合が増えうる欄（段階、状態、結果など）は、知らない値も文字列のまま通す。
 * - 形が違うもの（欄がない、型が違う、知らない形式の版）は ProtocolError を投げる。
 * - ただし、セッションの要約とランチャーの事象は部分ごとに読む（readSessionSummary、readServerMessage）。
 *   ランチャーから来る部分（照合、結果、所見、participant）や、サーバーが上限で縮めた部分が読めなくても、
 *   読める部分は使い、読めない部分は既定の値にして、その名前を `unreadable` に入れる。要約の芯（id、state、
 *   launcher、stop）と事象の芯（seq、event）が読めないときだけ、全体を読めないとする。
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

/**
 * このライブラリが読む GUI サーバーの口の版（`api/app` と `hello` の `vehicle_sil_gui`）。違う版の hello と api/app は
 * 読まない（欄の意味が変わっているかもしれないため）。版を上げる決まりは gui/protocol/README.md の「版と互換」。
 */
export const PROTOCOL_VERSION = 1;

// --- 読み取りの部品 ---------------------------------------------------------------------------

/** 届いた値を読む関数。path は誤りの場所の表示（例: `message.session`）。形が違えば ProtocolError を投げる。 */
export type Reader<T> = (value: unknown, path: string) => T;
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

/**
 * 部分ごとに読むときに、読めなかった部分の名前を集める。名前は root からの相対で、配列の添え字を除く
 * （`message.session.participants[2].signal` → `participants.signal`）。
 */
export class Unreadable {
  private readonly names = new Set<string>();

  constructor(
    private readonly root: string,
    private readonly prefix = '',
  ) {}

  add(path: string): void {
    const relative = path.startsWith(`${this.root}.`) ? path.slice(this.root.length + 1) : path;
    this.names.add(this.prefix + relative.replace(/\[\d+\]/g, ''));
  }

  get parts(): string[] {
    return [...this.names];
  }
}

/** 読めなければ fallback にして、その場所を u に覚える（u がなければ投げる）。欄がないのも読めないうち。 */
function lenient<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  if (!u) return read;
  return (value, path) => {
    try {
      return read(value, path);
    } catch (error) {
      if (!(error instanceof ProtocolError)) throw error;
      u.add(error.path);
      return fallback;
    }
  };
}

/** 欄がなければ黙って fallback、型が違えば fallback にして u に覚える（u がなければ投げる）。 */
function soft<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  return lenient(optional(read, fallback), fallback, u);
}

/** 配列。u があれば、読めない要素を読み飛ばして u に覚える（配列でないか、ないときは空）。 */
function items<T>(read: Reader<T>, u?: Unreadable): Reader<T[]> {
  if (!u) return array(read);
  return (value, path) => {
    if (!Array.isArray(value)) {
      u.add(path);
      return [];
    }
    const out: T[] = [];
    value.forEach((item, i) => {
      try {
        out.push(read(item, `${path}[${i}]`));
      } catch (error) {
        if (!(error instanceof ProtocolError)) throw error;
        u.add(error.path);
      }
    });
    return out;
  };
}

/**
 * 形式の版の欄。知らない版は読まない（意味が変わっているかもしれないため）。部分ごとに読むとき（u）は、
 * 読めない部分として覚えて、読める欄は読む。
 */
function version(
  fields: Fields,
  path: string,
  key: string,
  expected: number,
  u?: Unreadable,
): void {
  if (fields[key] !== expected) {
    const error = new ProtocolError(`${path}.${key}`, `format version ${expected}`);
    if (!u) throw error;
    u.add(error.path);
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

/** 所見を読む。u があれば部分ごとに（重さと文が読めなければ、その所見は読めない）。 */
export const readFinding = (value: unknown, path: string, u?: Unreadable): Finding => {
  const f = object(value, path);
  return {
    severity: field(f, path, 'severity', string),
    code: field(f, path, 'code', lenient(string, '', u)),
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    participant: field(f, path, 'participant', soft(nullable(string), null, u)),
    network: field(f, path, 'network', soft(nullable(string), null, u)),
    message: field(f, path, 'message', string),
    detail: field(f, path, 'detail', soft(nullable(string), null, u)),
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
  /** ECU が申告した変数の出し方。申告がない、照合していない、古いランチャーでは null。 */
  control: ControlOffer | null;
}

const readCheckParticipant = (value: unknown, path: string, u?: Unreadable): CheckParticipant => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', nullable(string)),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    required: field(f, path, 'required', lenient(nullable(boolean), null, u)),
    enabled: field(f, path, 'enabled', lenient(nullable(boolean), null, u)),
    step_ms: field(f, path, 'step_ms', lenient(nullable(number), null, u)),
    status: field(f, path, 'status', lenient(string, '', u)),
    variables: field(
      f,
      path,
      'variables',
      soft(nullable(u ? items(readVariable, u) : array(readVariable)), null, u),
    ),
    control: field(
      f,
      path,
      CONTROL_CHECK_KEY,
      soft(
        nullable((v, p) => readControlOffer(v, p, u)),
        null,
        u,
      ),
    ),
  };
};

export interface CheckNetwork {
  name: string | null;
  kind: string | null;
  /** 使う participant 名（構成ファイルの順）。 */
  participants: string[];
}

const readCheckNetwork = (value: unknown, path: string, u?: Unreadable): CheckNetwork => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', lenient(nullable(string), null, u)),
    kind: field(f, path, 'kind', lenient(nullable(string), null, u)),
    participants: field(f, path, 'participants', items(string, u)),
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

/** 照合の結果を読む。u があれば部分ごとに。 */
export const readCheckResult = (value: unknown, path: string, u?: Unreadable): CheckResult => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_check', 1, u);
  return {
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    result: field(f, path, 'result', lenient(string, '', u)),
    errors: field(f, path, 'errors', lenient(integer, 0, u)),
    warnings: field(f, path, 'warnings', lenient(integer, 0, u)),
    notes: field(f, path, 'notes', lenient(integer, 0, u)),
    stop_at_ms: field(f, path, 'stop_at_ms', soft(nullable(number), null, u)),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readCheckParticipant(v, p, u), u),
    ),
    networks: field(
      f,
      path,
      'networks',
      items((v, p) => readCheckNetwork(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
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
  /** 始まったときの、変数を読み書きする口のつながり方（古いランチャーは書かないので null）。 */
  control_link: ControlLink | null;
}

/**
 * 変数を読み書きする口（ランチャーの control-links）のつながり方。ランチャーの語はここにだけ書く（名前が増えたり
 * 変わったりしても、知らない値は文字列のまま通す）。
 * connected: 変数を読み書きできる、no_hello: 出すと申告したのに hello が来なかった、version_mismatch: 形式の版が
 * 違う、not_offered: 変数はあるが出すと申告していない（--no-control、古い実行ファイル）、none: 変数を出す ECU では
 * ない。
 */
export type ControlState = Open<
  'connected' | 'no_hello' | 'version_mismatch' | 'not_offered' | 'none'
>;

/** ランチャーの事象の名前（変数を読み書きする口のつながり方）。 */
export const CONTROL_EVENT = 'control_link';
/** ランチャーの結果の participants[] の、口のつながり方のキー（オブジェクト）。 */
export const CONTROL_RESULT_KEY = 'control_link';
/** GUI サーバーの要約の participants[] の、口のつながり方の欄（事象の欄を重ねたもの）。 */
export const CONTROL_SUMMARY_KEY = 'control_link';
/** 照合の結果の participants[] の、ECU が申告した変数の出し方の欄。 */
export const CONTROL_CHECK_KEY = 'control';

/** ECU が申告した、変数の出し方（照合の participants[].control）。 */
export interface ControlOffer {
  /** 運び手（いまは silkit）。 */
  transport: string;
  /** メッセージの形式の版。 */
  wire_version: number | null;
}

const readControlOffer = (value: unknown, path: string, u?: Unreadable): ControlOffer => {
  const f = object(value, path);
  return {
    transport: field(f, path, 'transport', lenient(string, '', u)),
    wire_version: field(f, path, 'wire_version', soft(nullable(integer), null, u)),
  };
};

/** ランチャーの control_link の事象と結果（と、要約の participant の control_link）: ある participant の口のつながり方。 */
export interface ControlLink {
  state: ControlState;
  /** connected: その ECU が出す変数の数。 */
  variables: number | null;
  /** version_mismatch: 相手の形式の版。 */
  version: number | null;
  /** 約束より遅れて届いたメッセージの数（結果だけ。事象にはないので null）。 */
  late_messages: number | null;
}

const readControlLink = (value: unknown, path: string, u?: Unreadable): ControlLink => {
  const f = object(value, path);
  return {
    state: field(f, path, 'state', string),
    variables: field(f, path, 'variables', soft(nullable(integer), null, u)),
    version: field(f, path, 'version', soft(nullable(integer), null, u)),
    late_messages: field(f, path, 'late_messages', soft(nullable(integer), null, u)),
  };
};

const readRunParticipant = (value: unknown, path: string, u?: Unreadable): RunParticipant => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    required: field(f, path, 'required', lenient(boolean, true, u)),
    enabled: field(f, path, 'enabled', lenient(boolean, true, u)),
    ended: field(f, path, 'ended', lenient(string, '', u)),
    exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
    signal: field(f, path, 'signal', soft(nullable(string), null, u)),
    left_processes: field(f, path, 'left_processes', soft(boolean, false, u)),
    control_link: field(
      f,
      path,
      CONTROL_RESULT_KEY,
      soft(
        nullable((v, p) => readControlLink(v, p, u)),
        null,
        u,
      ),
    ),
  };
};

/** 実行の結果の名前（`result` 事象の `result.result`）。 */
export type RunResultName = Open<
  'passed' | 'failed' | 'interrupted' | 'check_failed' | 'unreadable' | 'setup_failed' | 'usage'
>;

/**
 * 結果の timing: 時間の進み方（数の定義はランチャーの docs の「等速で動かす」）。何も起動しなかったとき、古い
 * ランチャーでは結果に null。欄ごとに、ないか読めなければ null。
 */
export interface RunTiming {
  /** 終わったときの速さ。 */
  speed: Speed;
  /** controller の刻み（ミリ秒）。ステップ送りはこの倍数に切り上がる。 */
  time_step_ms: number | null;
  ecu_step_ms: number | null;
  /** 刻みが ECU のステップより粗いために、ECU のステップが遅れて動く量の最大（ミリ秒）。 */
  step_delay_ms: number | null;
  late_tolerance_ms: number | null;
  sim_ms: number | null;
  /** かかった実時間（一時停止していた時間を含む）。 */
  wall_ms: number | null;
  actual_speed: number | null;
  /** 速さを合わせた刻みの数。0 なら最速で動いた（遅れは数えていない）。 */
  paced_steps: number | null;
  late_steps: number | null;
  max_late_ms: number | null;
  slipped_ms: number | null;
  late_ms: number | null;
  paused_ms: number | null;
}

const readRunTiming = (value: unknown, path: string, u?: Unreadable): RunTiming => {
  const f = object(value, path);
  const count = (key: string) => field(f, path, key, soft(nullable(number), null, u));
  return {
    speed: field(f, path, 'speed', soft(readSpeed, null, u)),
    time_step_ms: count('time_step_ms'),
    ecu_step_ms: count('ecu_step_ms'),
    step_delay_ms: count('step_delay_ms'),
    late_tolerance_ms: count('late_tolerance_ms'),
    sim_ms: count('sim_ms'),
    wall_ms: count('wall_ms'),
    actual_speed: count('actual_speed'),
    paced_steps: count('paced_steps'),
    late_steps: count('late_steps'),
    max_late_ms: count('max_late_ms'),
    slipped_ms: count('slipped_ms'),
    late_ms: count('late_ms'),
    paused_ms: count('paused_ms'),
  };
};

/** `vehicle_sil_launch run --json` の結果（形式の版 1）。 */
export interface RunResult {
  file: string | null;
  result: RunResultName;
  errors: number;
  warnings: number;
  notes: number;
  simulation: string | null;
  interrupted_by: string | null;
  /** 時間の進み方。古いランチャーと、何も起動しなかったときは null。 */
  timing: RunTiming | null;
  participants: RunParticipant[];
  findings: Finding[];
  check: CheckResult | null;
}

/**
 * 実行の結果を読む。u があれば部分ごとに: サーバーが上限で縮めた結果（入れ子でない短い欄だけ）も、結果として
 * 読む（participant と所見は空にして、読めない部分に入れる）。結果の名前が読めなければ ''。
 */
export const readRunResult = (value: unknown, path: string, u?: Unreadable): RunResult => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_run', 1, u);
  return {
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    result: field(f, path, 'result', lenient(string, '', u)),
    errors: field(f, path, 'errors', lenient(integer, 0, u)),
    warnings: field(f, path, 'warnings', lenient(integer, 0, u)),
    notes: field(f, path, 'notes', lenient(integer, 0, u)),
    simulation: field(f, path, 'simulation', soft(nullable(string), null, u)),
    interrupted_by: field(f, path, 'interrupted_by', soft(nullable(string), null, u)),
    timing: field(
      f,
      path,
      'timing',
      soft(
        nullable((v, p) => readRunTiming(v, p, u)),
        null,
        u,
      ),
    ),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readRunParticipant(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
    check: field(
      f,
      path,
      'check',
      soft(
        nullable((v, p) => readCheckResult(v, p, u)),
        null,
        u,
      ),
    ),
  };
};

// --- ランチャー: 経過の事象（run --events の 1 行） ---------------------------------------------

/** 段階。この順に進み、飛ばすことはあるが戻らない。 */
export const PHASES = ['checking', 'starting', 'waiting', 'running', 'stopping', 'ended'] as const;
export type Phase = Open<(typeof PHASES)[number]>;

export type ParticipantState = Open<
  | 'disabled'
  | 'not_started'
  | 'started'
  | 'joined'
  | 'failed'
  | 'ended'
  | 'stop_requested'
  | 'killed'
>;

interface EventBase {
  seq: number;
  /** ランチャーの開始からの実時間（ミリ秒）。ない事象もある（サーバーは欄がなくても通す）。 */
  t_ms: number | null;
  /** 読めなかった欄の名前（`finding.code` のように事象の種類を先に付ける）。なければ欄がない。 */
  unreadable?: string[];
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
  | (EventBase & { event: 'check'; check: CheckResult | null })
  | (EventBase & { event: 'registry'; uri: string | null })
  | (EventBase & {
      event: 'participant';
      name: string;
      state: ParticipantState;
      pid: number | null;
      exit_code: number | null;
      signal: string | null;
      /** failed: なぜ失敗としたか（left_simulation、process_ended、ended_before_start。増えることがある）。 */
      reason: string | null;
      /** not_started: 起動できなかった理由。failed: その所見の文（決まっていれば）。 */
      message: string | null;
    })
  | (EventBase & { event: 'log'; source: string; line: string })
  /** 変数を読み書きする口のつながり方（始まる直前に、起動した participant ごとに 1 回）。 */
  | (EventBase & { event: typeof CONTROL_EVENT; name: string; link: ControlLink })
  /** 読む側が遅くて、ランチャーが書けなかった log の行の数（出どころごと）。その位置に来る。 */
  | (EventBase & { event: 'log_dropped'; count: number; sources: Record<string, number> })
  | (EventBase & { event: 'finding'; finding: Finding })
  | (EventBase & { event: 'time' } & TimeReport)
  | (EventBase & { event: 'command' } & CommandReport)
  | (EventBase & { event: 'result'; result: RunResult });

/**
 * time の事象（と要約の time）: controller が見ている仮想時間と、その進み方。古いランチャーは sim_ms だけを書くので、
 * ほかの欄はないか読めなければ null（画面は、null なら出さない）。
 */
export interface TimeReport {
  /** 仮想時間（ミリ秒。減らない）。 */
  sim_ms: number;
  /** いまの速さ（数か 'max'）。書いていなければ null。 */
  speed: Speed;
  /** 一時停止しているか（止まる予定の時刻に着いたら true）。書いていなければ null。 */
  paused: boolean | null;
  /** 直近 1 秒ほどの実際の速さ。動いた時間が短いとき、一時停止しているときは null。 */
  actual_speed: number | null;
  /** それまでに遅れた刻みの数（いま終わったとしたときの値。最速では 0）。 */
  late_steps: number | null;
  /** それまでの最大の遅れ（ミリ秒）。 */
  max_late_ms: number | null;
  /** それまでのずれの合計（ミリ秒）。 */
  slipped_ms: number | null;
  /** 期限を過ぎてもまだ始まっていない刻みの、いまの遅れ（ミリ秒。なければ 0）。 */
  late_ms: number | null;
}

function readTimeFields(f: Fields, path: string, u?: Unreadable): TimeReport {
  const count = (key: string) => field(f, path, key, soft(nullable(number), null, u));
  return {
    sim_ms: field(f, path, 'sim_ms', number),
    speed: field(f, path, 'speed', soft(readSpeed, null, u)),
    paused: field(f, path, 'paused', soft(nullable(boolean), null, u)),
    actual_speed: count('actual_speed'),
    late_steps: count('late_steps'),
    max_late_ms: count('max_late_ms'),
    slipped_ms: count('slipped_ms'),
    late_ms: count('late_ms'),
  };
}

/** command の事象（と要約の last_command）: ランチャーが標準入力の命令を受けたか。 */
export interface CommandReport {
  /** 命令の名前（読めなかった行は null）。 */
  command: string | null;
  /** stdin: 行で来た、eof: 標準入力の終わり。 */
  source: Open<'stdin' | 'eof'>;
  accepted: boolean;
  /** 受け付けなかった理由（英語のまま出す）。受け付けたら null。 */
  message: string | null;
  /** 受け付けた step の、実際に進む仮想時間（刻みの倍数に切り上げたもの）。ほかは null。 */
  ms: number | null;
}

/** 要約の last_command: 事象の欄と、その事象の seq（古いサーバーは書かないので null）。 */
export type SummaryCommand = CommandReport & { seq: number | null };

function readCommandFields(f: Fields, path: string, u?: Unreadable): CommandReport {
  return {
    command: field(f, path, 'command', nullable(string)),
    source: field(f, path, 'source', soft(string, 'stdin', u)),
    accepted: field(f, path, 'accepted', boolean),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
    ms: field(f, path, 'ms', soft(nullable(integer), null, u)),
  };
}

/**
 * ランチャーの事象を 1 つ読む。知らない事象は null（読み飛ばす）。partly が真なら部分ごとに読み、読めない欄は
 * 既定の値にして、その名前を unreadable に入れる（芯の seq と event、事象ごとの芯の欄が読めないときだけ投げる）。
 */
export const readLauncherEvent = (
  value: unknown,
  path: string,
  partly = false,
): LauncherEvent | null => {
  const f = object(value, path);
  const kind = field(f, path, 'event', string);
  const u = partly ? new Unreadable(path, `${kind}.`) : undefined;
  const event = readEventFields(
    f,
    path,
    kind,
    {
      seq: field(f, path, 'seq', integer),
      t_ms: field(f, path, 't_ms', soft(nullable(number), null, u)),
    },
    u,
  );
  if (event && u && u.parts.length > 0) event.unreadable = u.parts;
  return event;
};

function readEventFields(
  f: Fields,
  path: string,
  kind: string,
  base: EventBase,
  u?: Unreadable,
): LauncherEvent | null {
  switch (kind) {
    case 'hello':
      // 形式の版の食い違いはサーバーが見つけて知らせる。画面は読める欄だけを使う
      return {
        ...base,
        event: kind,
        vehicle_sil_events: field(f, path, 'vehicle_sil_events', lenient(integer, 0, u)),
        vehicle_sil: field(f, path, 'vehicle_sil', lenient(string, '', u)),
        pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
        file: field(f, path, 'file', soft(nullable(string), null, u)),
        arguments: field(f, path, 'arguments', soft(items(string, u), [], u)),
      };
    case 'phase':
      return { ...base, event: kind, phase: field(f, path, 'phase', string) };
    case 'check':
      // 読めない照合は null（読めない部分として出し、ほかの事象は当てる）
      return {
        ...base,
        event: kind,
        check: field(
          f,
          path,
          'check',
          lenient((v, p) => readCheckResult(v, p, u), null, u),
        ),
      };
    case 'registry':
      return { ...base, event: kind, uri: field(f, path, 'uri', soft(nullable(string), null, u)) };
    case 'participant':
      return {
        ...base,
        event: kind,
        name: field(f, path, 'name', string),
        state: field(f, path, 'state', string),
        pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
        exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
        signal: field(f, path, 'signal', soft(nullable(string), null, u)),
        reason: field(f, path, 'reason', soft(nullable(string), null, u)),
        message: field(f, path, 'message', soft(nullable(string), null, u)),
      };
    case 'log_dropped':
      return {
        ...base,
        event: kind,
        count: field(f, path, 'count', integer),
        sources: field(f, path, 'sources', record(integer)),
      };
    case CONTROL_EVENT:
      return {
        ...base,
        event: kind,
        name: field(f, path, 'name', string),
        link: readControlLink(f, path, u),
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
      return { ...base, event: kind, finding: readFinding(f, path, u) };
    case 'time':
      return { ...base, event: kind, ...readTimeFields(f, path, u) };
    case 'command':
      return { ...base, event: kind, ...readCommandFields(f, path, u) };
    case 'result':
      return {
        ...base,
        event: kind,
        result: field(f, path, 'result', (v, p) => readRunResult(v, p, u)),
      };
    default:
      return null;
  }
}

// --- GUI サーバー: 共通 -------------------------------------------------------------------------

/** サーバーの知らせ（API の誤り、WebSocket の error、セッションの problems）。message は英語のまま出す。 */
export interface ServerProblem {
  code: string;
  message: string;
  /** launcher-stderr の、ここで落とした行の数（`detail.skipped`）。ほかの知らせにはない。 */
  skipped?: number;
}

/** サーバーの知らせ（`{code, message, detail}`）を読む。HTTP の誤りの `error` の中身も同じ形。 */
export const readServerProblem: Reader<ServerProblem> = (value, path) => {
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
 * 速さ。'max' は最速、数は倍率（1 が等速）。null は、始めるときの指定ではランチャーの既定（構成ファイルの値）、
 * ランチャーが書く値では「書いていない」。
 */
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
    /** ステップ送りの ms の上限。古いサーバーは書かないので、ランチャーの上限（10^12）。 */
    step_ms_max: number;
    /** 照合の時間の上限（秒。`--check-timeout`）。古いサーバーは書かないので、その既定（120）。 */
    check_timeout_s: number;
    /** 変数の watch: 間隔（ミリ秒）の下限と上限、接続ごとの数、1 つに名前で挙げられる変数の数。 */
    watch_period_ms_min: number;
    watch_period_ms_max: number;
    watches: number;
    watch_variables: number;
    /** 1 回の書き込みの変数の数の上限と、書き込みに指定できる仮想時刻（ミリ秒）の上限。 */
    writes_per_request: number;
    write_at_ms_max: number;
  };
  /**
   * いま実行を始めたら変数を読み書きできるか（ランチャーが Control Server を持ち、GUI サーバーが gRPC を持つ）と、
   * できない理由（英語のまま出す）。古いサーバーは書かない（available は false、reason は null）。
   */
  variables: { available: boolean; reason: string | null };
}

export const readAppInfo = (value: unknown): AppInfo => {
  const path = 'app';
  const f = object(value, path);
  version(f, path, 'vehicle_sil_gui', PROTOCOL_VERSION);
  const lp = `${path}.launcher`;
  const launcher = field(f, path, 'launcher', object);
  const launcherVersion = field(launcher, lp, 'version', nullable(object));
  const limits = field(f, path, 'limits', object);
  const lim = `${path}.limits`;
  const variables = field(f, path, 'variables', optional(object, null));
  return {
    version: field(f, path, 'version', string),
    instance: field(f, path, 'instance', string),
    root: field(f, path, 'root', string),
    initial_file: field(f, path, 'initial_file', nullable(string)),
    launcher: {
      path: field(launcher, lp, 'path', nullable(string)),
      usable: field(launcher, lp, 'usable', boolean),
      problems: field(launcher, lp, 'problems', array(readServerProblem)),
      features: launcherVersion
        ? field(launcherVersion, `${lp}.version`, 'features', optional(array(string), []))
        : [],
    },
    limits: {
      speed_min: field(limits, `${path}.limits`, 'speed_min', number),
      speed_max: field(limits, `${path}.limits`, 'speed_max', number),
      step_ms_max: field(limits, `${path}.limits`, 'step_ms_max', optional(number, 1e12)),
      check_timeout_s: field(limits, `${path}.limits`, 'check_timeout_s', optional(number, 120)),
      watch_period_ms_min: field(limits, lim, 'watch_period_ms_min', optional(number, 20)),
      watch_period_ms_max: field(limits, lim, 'watch_period_ms_max', optional(number, 60_000)),
      write_at_ms_max: field(limits, lim, 'write_at_ms_max', optional(number, 1e12)),
      watches: field(limits, lim, 'watches', optional(number, 16)),
      watch_variables: field(limits, lim, 'watch_variables', optional(number, 2000)),
      writes_per_request: field(limits, lim, 'writes_per_request', optional(number, 256)),
    },
    variables: variables
      ? {
          available: field(variables, `${path}.variables`, 'available', boolean),
          reason: field(variables, `${path}.variables`, 'reason', nullable(string)),
        }
      : { available: false, reason: null },
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
  /** failed の事象の reason（失敗とした理由。後で ended が来ても残る）。 */
  reason: string | null;
  /** not_started の理由、failed の所見の文。 */
  message: string | null;
  /** 大きすぎて要約に入れなかった欄の名前（後の事象でその欄が届けば消える）。 */
  omitted_fields: string[];
  /** ランチャーが止めるよう求めた（stop_requested の事象が来た。後の状態でも残る）。 */
  stop_requested: boolean;
  /** 変数を読み書きする口のつながり方（ランチャーの control_link の事象）。まだなければ null。 */
  control_link: ControlLink | null;
}

const readSummaryParticipant = (
  value: unknown,
  path: string,
  u: Unreadable,
): SummaryParticipant => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    state: field(f, path, 'state', soft(nullable(string), null, u)),
    pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
    exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
    signal: field(f, path, 'signal', soft(nullable(string), null, u)),
    reason: field(f, path, 'reason', soft(nullable(string), null, u)),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
    omitted_fields: field(f, path, 'omitted_fields', soft(items(string, u), [], u)),
    stop_requested: field(f, path, 'stop_requested', soft(boolean, false, u)),
    control_link: field(
      f,
      path,
      CONTROL_SUMMARY_KEY,
      soft(
        nullable((v, p) => readControlLink(v, p, u)),
        null,
        u,
      ),
    ),
  };
};

/** 要約の launcher: ランチャーのプロセスが動いているか、終わったならその終わり方。 */
export interface SummaryLauncher {
  /** 動いているか。結果を出した後も、プロセスが終わるまでは true。止められるかはこれで決める。 */
  running: boolean;
  exit_code: number | null;
  /** 終わらせたシグナルの番号（POSIX だけ）。 */
  signal: number | null;
  /** 動いているのに、この秒数より長く何も書いていない（心拍も来ない。止まっているかもしれない）。なければ null。 */
  silent_s: number | null;
}

const readSummaryLauncher: Reader<SummaryLauncher> = (value, path) => {
  const f = object(value, path);
  return {
    running: field(f, path, 'running', boolean),
    exit_code: field(f, path, 'exit_code', nullable(integer)),
    signal: field(f, path, 'signal', nullable(integer)),
    silent_s: field(f, path, 'silent_s', optional(nullable(integer), null)),
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
  /** 始めたときに指定した速さ（null は構成ファイルのまま）。いまの速さは time.speed。 */
  speed: Speed;
  /** 止めた状態で始めたか。 */
  start_paused: boolean;
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
  /** 最後の time 事象（仮想時間、速さ、一時停止、遅れの数）。まだなければ null。 */
  time: TimeReport | null;
  /** 最後の command 事象（受け付けたか、理由）。まだなければ null。 */
  last_command: SummaryCommand | null;
  /**
   * この run の照合の結果（check の事象）。まだなければ null。`session_without_check` を頼んだ接続の `session` の
   * 通知には欄がなく、null として読む（手元の照合の結果をそのまま使う）。
   */
  check: CheckResult | null;
  participants: SummaryParticipant[];
  findings: Finding[];
  result: RunResult | null;
  /** 出どころごとの、サーバーが覚えている行と落とした行の数。 */
  log: Record<string, LogCounts>;
  problems: ServerProblem[];
  problems_omitted: number;
  /**
   * 読めなかった部分の名前（`result.participants`、`findings.code` など。画面が付ける。サーバーの欄ではない）。
   * 上限で縮めた結果の、ない欄もここに入る。
   */
  unreadable: string[];
}

/**
 * セッションの要約を、部分ごとに読む。芯（id、state、launcher、stop）が読めなければ投げる。ほかの部分は、
 * 読めなければ既定の値（null、空）にして、その名前を unreadable に入れる。
 */
export const readSessionSummary: Reader<SessionSummary> = (value, path) => {
  const f = object(value, path);
  const u = new Unreadable(path);
  const time = field(f, path, 'time', soft(nullable(object), null, u));
  const timePath = `${path}.time`;
  const summary: Omit<SessionSummary, 'unreadable'> = {
    id: field(f, path, 'id', integer),
    state: field(f, path, 'state', string),
    launcher: field(f, path, 'launcher', readSummaryLauncher),
    stop: field(f, path, 'stop', string),
    file: field(f, path, 'file', lenient(string, '', u)),
    disable: field(f, path, 'disable', items(string, u)),
    speed: field(f, path, 'speed', lenient(readSpeed, null, u)),
    start_paused: field(f, path, 'start_paused', soft(boolean, false, u)),
    phase: field(f, path, 'phase', soft(nullable(string), null, u)),
    last_seq: field(f, path, 'last_seq', soft(nullable(integer), null, u)),
    last_t_ms: field(f, path, 'last_t_ms', soft(nullable(number), null, u)),
    sim_ms: time ? field(time, timePath, 'sim_ms', lenient(number, null, u)) : null,
    time_omitted_fields: time
      ? field(time, timePath, 'omitted_fields', soft(items(string, u), [], u))
      : [],
    time: time
      ? lenient((v, p) => readTimeFields(object(v, p), p, u), null, u)(time, timePath)
      : null,
    last_command: field(
      f,
      path,
      'last_command',
      soft(
        nullable((v, p): SummaryCommand => {
          const c = object(v, p);
          return {
            ...readCommandFields(c, p, u),
            seq: field(c, p, 'seq', soft(nullable(integer), null, u)),
          };
        }),
        null,
        u,
      ),
    ),
    check: field(
      f,
      path,
      'check',
      soft(
        nullable((v, p) => readCheckResult(v, p, u)),
        null,
        u,
      ),
    ),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readSummaryParticipant(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
    result: field(
      f,
      path,
      'result',
      soft(
        nullable((v, p) => readRunResult(v, p, u)),
        null,
        u,
      ),
    ),
    log: field(f, path, 'log', soft(partlyRecord(readLogCounts, u), {}, u)),
    problems: field(f, path, 'problems', items(readServerProblem, u)),
    problems_omitted: field(f, path, 'problems_omitted', soft(integer, 0, u)),
  };
  return { ...summary, unreadable: u.parts };
};

/** 出どころごとの数を、読めるものだけ読む。 */
function partlyRecord<T>(read: Reader<T>, u: Unreadable): Reader<Record<string, T>> {
  return (value, path) => {
    const out: Record<string, T> = {};
    for (const [key, item] of Object.entries(object(value, path))) {
      const read1 = lenient(read, undefined, u)(item, `${path}.${key}`);
      if (read1 !== undefined) out[key] = read1;
    }
    return out;
  };
}

/** GET api/session、POST api/run、POST api/stop の応答（いまのセッション。なければ null）。 */
export const readSessionResponse = (value: unknown): SessionSummary | null =>
  field(object(value, 'response'), 'response', 'session', nullable(readSessionSummary));

/** POST api/stop の本文。force: false は順に止める（何度頼んでも同じ）、true は待たずに止める。 */
export interface StopRequestBody {
  force: boolean;
}

/** 時刻の命令の名前（ランチャーの標準入力の命令のうち、stop を除くもの）。 */
export type TimeCommandName = 'pause' | 'resume' | 'step' | 'speed';

/**
 * POST api/time の本文。サーバーはランチャーに 1 行で渡して、すぐ返す。受け付けたかは command の事象で、効いたことは
 * time の事象で届く。
 */
export type TimeCommand =
  | { command: 'pause' }
  | { command: 'resume' }
  /** 一時停止している間だけ。ms 進めて、また止まる（ランチャーが刻みの倍数に切り上げる）。 */
  | { command: 'step'; ms: number }
  | { command: 'speed'; value: number | 'max' };

/** POST api/time の応答: 送った実行の番号と、ランチャーに渡した命令。 */
export interface TimeAnswer {
  session_id: number;
  command: TimeCommandName;
}

export const readTimeResponse = (value: unknown): TimeAnswer => {
  const path = 'response';
  const f = object(value, path);
  const command = field(f, path, 'command', object);
  return {
    session_id: field(f, path, 'session_id', integer),
    command: field(command, `${path}.command`, 'command', string) as TimeCommandName,
  };
};

// --- GUI サーバー: WebSocket -------------------------------------------------------------------

/** つないだ直後に送る最初のメッセージ。session と from_seq を言うと、その続きから再送される。 */
/** 変数 1 つの定義（要約の `variables.peers[].variables[]`）。 */
export interface VariableSummary {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  role: Open<'input' | 'output' | 'parameter'>;
  writable: boolean;
  unit: string;
  min: number | null;
  max: number | null;
  step: number | null;
  description: string;
  /** 列挙の項目（type が enum のとき。ほかは null）。 */
  enum: { name: string; value: number }[] | null;
}

/** 書き込みの結果（`operation` の通知と、要約の `variables.operations[]`）。 */
export interface OperationReport {
  seq: number;
  /** sent、queued（途中）。確定したら applied、late、rejected、cancelled、invalid、busy、unknown、mixed。 */
  state: string;
  done: boolean;
  peers: Record<
    string,
    { state: string; applied_at_ms: number | null; late: boolean; reason: string | null }
  >;
}

/** 要約の `variables`: 変数の機能の状態、participant ごとの変数、最近の操作。 */
export interface VariablesSummary {
  state: Open<'off' | 'connecting' | 'ready' | 'failed' | 'ended'>;
  reason: string | null;
  peers: { name: string; connected: boolean; variables: VariableSummary[] }[];
  peers_omitted: number;
  variables_omitted: number;
  /** 未確定の操作の全部（サーバーが覚えている分）と、最近の操作。 */
  operations: OperationReport[];
  /** この番号以上の操作は、全部 operations に入っている（言わないサーバーでは null）。 */
  operations_from_seq: number | null;
  /** サーバーが覚えきれなかった未確定の操作の数（ふつうは 0）。 */
  operations_open_omitted: number;
}

const readOperationReport: Reader<OperationReport> = (value, path) => {
  const f = object(value, path);
  return {
    seq: field(f, path, 'seq', integer),
    state: field(f, path, 'state', string),
    done: field(f, path, 'done', boolean),
    peers: field(
      f,
      path,
      'peers',
      record((v, p) => {
        const t = object(v, p);
        return {
          state: field(t, p, 'state', string),
          applied_at_ms: field(t, p, 'applied_at_ms', optional(nullable(number), null)),
          late: field(t, p, 'late', optional(boolean, false)),
          reason: field(t, p, 'reason', optional(nullable(string), null)),
        };
      }),
    ),
  };
};

const readVariableSummary: Reader<VariableSummary> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', string),
    role: field(f, path, 'role', optional(string, 'output')),
    writable: field(f, path, 'writable', optional(boolean, false)),
    unit: field(f, path, 'unit', optional(string, '')),
    min: field(f, path, 'min', optional(nullable(number), null)),
    max: field(f, path, 'max', optional(nullable(number), null)),
    step: field(f, path, 'step', optional(nullable(number), null)),
    description: field(f, path, 'description', optional(string, '')),
    enum: field(
      f,
      path,
      'enum',
      optional(
        nullable(
          array((v, p) => {
            const item = object(v, p);
            return { name: field(item, p, 'name', string), value: field(item, p, 'value', number) };
          }),
        ),
        null,
      ),
    ),
  };
};

/** 要約の `variables` を読む（`hello` と HTTP の要約にある。`session` の通知にはない）。 */
export const readVariablesSummary = (value: unknown): VariablesSummary =>
  readVariables(value, 'variables');

const readVariables: Reader<VariablesSummary> = (value, path) => {
  const f = object(value, path);
  return {
    state: field(f, path, 'state', string),
    reason: field(f, path, 'reason', optional(nullable(string), null)),
    peers: field(
      f,
      path,
      'peers',
      optional(
        array((v, p) => {
          const peer = object(v, p);
          return {
            name: field(peer, p, 'name', string),
            connected: field(peer, p, 'connected', optional(boolean, false)),
            variables: field(peer, p, 'variables', optional(array(readVariableSummary), [])),
          };
        }),
        [],
      ),
    ),
    peers_omitted: field(f, path, 'peers_omitted', optional(integer, 0)),
    variables_omitted: field(f, path, 'variables_omitted', optional(integer, 0)),
    operations: field(f, path, 'operations', optional(array(readOperationReport), [])),
    operations_from_seq: field(f, path, 'operations_from_seq', optional(integer, null)),
    operations_open_omitted: field(f, path, 'operations_open_omitted', optional(integer, 0)),
  };
};

/** `values` の通知: watch している変数の最後の値と、それがいつのものか。 */
export interface ValuesMessage {
  type: 'values';
  /** watch の id。 */
  id: number;
  session: number;
  /** controller の仮想時間（ミリ秒。わからなければ null）。 */
  sim_ms: number | null;
  /** true: 止まっていて、sim_ms より前に始まったステップの値は全部入っている。 */
  paused: boolean;
  /** participant ごとの、値を読んだステップの仮想時間と、変数の値（有限でない実数は null）。 */
  peers: Record<string, { sample_ms: number; values: Record<string, number | boolean | null> }>;
}

/** `api/write` の応答。 */
export interface WriteAnswer {
  session_id: number;
  /** 操作の番号。結果は `operation` の通知で届く。 */
  operation: number;
}

export const readWriteResponse = (value: unknown): WriteAnswer => {
  const f = object(value, 'response');
  return {
    session_id: field(f, 'response', 'session_id', integer),
    operation: field(f, 'response', 'operation', integer),
  };
};

/** 認証の後に画面が送る通知: 変数の watch を始める（同じ id なら置き換える）、やめる。 */
export type WatchMessage =
  | {
      type: 'watch';
      id: number;
      variables: 'all' | { peer: string; name: string }[];
      period_ms?: number;
    }
  | { type: 'unwatch'; id: number };

export interface AuthMessage {
  type: 'auth';
  token: string;
  session?: number;
  from_seq?: number;
  /**
   * true なら、この接続の `session` の通知の要約に `check` を入れない（`hello` の要約と `check` の事象で届く）。
   * 照合の結果は大きく、実行の間に変わらないため。
   */
  session_without_check?: boolean;
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
  | { type: 'resync'; message: string }
  /** 変数の機能の状態か一覧が変わった（実行が始まった直後にも 1 回）。 */
  | { type: 'variables'; session: number; variables: VariablesSummary }
  /** watch している変数の値。 */
  | ValuesMessage
  /** 書き込みの結果が変わった。 */
  | ({ type: 'operation'; session: number } & OperationReport);

/** WebSocket の通知を 1 つ読む。知らない通知と、知らない事象を包んだ通知は null（読み飛ばす）。 */
export const readServerMessage = (value: unknown): ServerMessage | null => {
  const path = 'message';
  const f = object(value, path);
  const type = field(f, path, 'type', string);
  switch (type) {
    case 'hello':
      version(f, path, 'vehicle_sil_gui', PROTOCOL_VERSION);
      return {
        type,
        instance: field(f, path, 'instance', string),
        session: field(f, path, 'session', nullable(readSessionSummary)),
        replay: field(f, path, 'replay', nullable(readReplay)),
      };
    case 'event': {
      const event = field(f, path, 'event', (v, p) => readLauncherEvent(v, p, true));
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
        ...readServerProblem(value, path),
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
    case 'variables':
      return {
        type,
        session: field(f, path, 'session', integer),
        variables: field(f, path, 'variables', readVariables),
      };
    case 'values':
      return {
        type,
        id: field(f, path, 'id', integer),
        session: field(f, path, 'session', integer),
        sim_ms: field(f, path, 'sim_ms', nullable(number)),
        paused: field(f, path, 'paused', boolean),
        peers: field(
          f,
          path,
          'peers',
          record((v, p) => {
            const peer = object(v, p);
            return {
              sample_ms: field(peer, p, 'sample_ms', number),
              values: field(
                peer,
                p,
                'values',
                record((x, xp) => {
                  if (x !== null && typeof x !== 'boolean') return number(x, xp);
                  return x;
                }),
              ),
            };
          }),
        ),
      };
    case 'operation':
      return {
        type,
        session: field(f, path, 'session', integer),
        ...readOperationReport(value, path),
      };
    default:
      return null;
  }
};
