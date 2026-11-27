import type { ConnectionState } from './connection';
import { HttpError, HttpRequest, HttpTransport, errorMessage } from './http';
import {
  Open,
  Unreadable,
  readAvailability,
  readOperationReport,
  readVariablesReport,
} from './protocol';
import type { VariableAvailability } from './protocol';
import { lenient } from './protocol/read';
import { onItsWay, simMsOf, standsAt } from './front-compose';
import { isHolding, isRunActive, type SessionView } from './session-state';
import { ErrorSink, Readable, Writable, rethrowLater } from './values';
import type { BindOptions, VariableBinding } from './variable-binding';

/**
 * Value of a variable: a number for reals, integers and enums, a boolean for bool. A non-finite real (NaN, infinity) is
 * null.
 */
export type VariableValue = number | boolean | null;

/** Names a variable: which ECU (peer), which variable. */
export interface VariableRef {
  peer: string;
  name: string;
}

/** Definition of a variable (from the ECU's declaration). */
export interface VariableInfo {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  /** input: writable during the run; parameter: writable only before the first step; output: read-only. */
  role: Open<'input' | 'output' | 'parameter'>;
  /** Whether it can be written (role is input or parameter). */
  writable: boolean;
  /** Whether vehicle-sil added the variable (`ecu.connected`, `peer.*`, `j1939.*`, `can.*`, `lin.*`, `eth.*`) rather than the ECU application declaring it. */
  builtin: boolean;
  unit: string;
  min: number | null;
  max: number | null;
  /** Suggested increment for editing (not enforced). */
  step: number | null;
  description: string;
  /** Items of an enum (when type is enum; null otherwise). */
  enum: { name: string; value: number }[] | null;
}

/** A participant that has variables. */
export interface VariablePeer {
  name: string;
  /** Whether it is connected to the Control Server now. */
  connected: boolean;
  variables: VariableInfo[];
}

/**
 * State of the variable API. unknown: not heard from the server yet; off: this run has no variables (reason says
 * why); connecting: connecting to the launcher's Control Server (also while reconnecting after a drop; reason says
 * why); ready: values can be read and written; failed: could not connect (reason says why); ended: the run has ended
 * (the values are the last ones).
 */
export type VariablesStatus = Open<'unknown' | 'off' | 'connecting' | 'ready' | 'failed' | 'ended'>;

/** List and state of the variables (of the current run). */
export interface VariablesState {
  /** Which run they belong to (null without a run). */
  session: string | null;
  status: VariablesStatus;
  /** Why the API is unavailable or cannot connect (the server's English text). null if none. */
  reason: string | null;
  peers: VariablePeer[];
  /**
   * Whether each ECU uses each variable now (participant, then variable name): information only (it changes neither
   * how the variable is read nor whether it can be written). Kept apart from `peers`, which holds the declared
   * definitions, so that a notification replaces this table and leaves `peers` as it is. Every variable in `peers` has
   * an entry; `active` when the server does not say.
   */
  availability: ReadonlyMap<string, ReadonlyMap<string, VariableAvailability>>;
  /** Number of participants and variables beyond the limit that the server did not keep. */
  peersOmitted: number;
  variablesOmitted: number;
}

/** One value read, and when it is from. */
export interface VariableReading {
  value: VariableValue;
  /** Virtual time of the step that read this value (ms). */
  sampleMs: number;
  /** The controller's virtual time when this value arrived (ms; null if unknown). */
  simMs: number | null;
  /**
   * Whether this is the value at the paused time. If true, this value includes the steps that began before simMs (it
   * may be shown as the value at the paused time). If false, it is the last value while running.
   */
  paused: boolean;
}

/**
 * The last value of each watched variable: `values[peer]?.[name]`. Variables whose value has not arrived yet are
 * absent.
 */
export type WatchValues = Readonly<Record<string, Readonly<Record<string, VariableReading>>>>;

/** One point of the history. */
export interface HistoryPoint {
  sampleMs: number;
  value: VariableValue;
}

/** Options of watch. */
export interface WatchOptions {
  /**
   * Interval at which values are received (ms; default 100). Never shorter than the server's lower limit
   * (watch_period_ms_min of limits, usually 20). If another watch observes the same variable at a shorter interval,
   * values arrive at that interval.
   */
  periodMs?: number;
  /**
   * Number of history points to keep per variable (default 0: none; limit HISTORY_MAX; a non-number or a negative
   * value counts as 0). The oldest are dropped first. Memory is taken as points arrive. The histories of all variables
   * together hold up to HISTORY_TOTAL_MAX points; once that is reached, every history keeps its current size and drops
   * its oldest points (so it holds fewer than requested). When the run changes, the history starts empty.
   */
  history?: number;
}

/** A watch of variables. Call close() when done with it (otherwise values keep arriving). */
export interface VariableWatch {
  /** The last values of the watched variables. A new object every time a value arrives. */
  readonly values: Readable<WatchValues>;
  /** The last value of the variable (undefined if none yet). */
  value(ref: VariableRef): VariableReading | undefined;
  /** History of the variable (oldest first, up to the number in WatchOptions.history). A new array on every call. */
  history(ref: VariableRef): HistoryPoint[];
  /** Stops. If no other watch observes the same variables, the server subscription is dropped too. */
  close(): void;
}

/** The server refused a watch. */
export interface WatchProblem {
  /** `invalid-watch` or `too-many-watches`. */
  code: string;
  /** The server's English text. */
  message: string;
}

/** Per-participant state of a write's result (operation). */
export interface OperationPeer {
  /** sent, queued, disconnected (in progress); applied, late, rejected, cancelled, invalid, busy, unknown (settled). */
  state: string;
  /** Virtual time of the step that applied it (ms). */
  appliedAtMs: number | null;
  /** Applied in a step after the requested time. */
  late: boolean;
  /** Why it went wrong (English text from the ECU or the Control Server). */
  reason: string | null;
  /**
   * How many of this participant's writes took effect, counted from the first (its writes are those
   * of the request for it, in order). null: not known.
   */
  appliedWrites: number | null;
  /** Position (from 0) of the write that `reason` is about. null: none, or not known. */
  failedWrite: number | null;
}

/** Result of a write (operation). */
export interface OperationView {
  session: string;
  /** Operation number (operation in the `api/write` answer). */
  seq: number;
  /**
   * Overall state: sent, queued (in progress). Once settled, the common state if every participant has the same one
   * (applied, late, rejected, cancelled, invalid, busy, unknown), mixed if they differ.
   */
  state: string;
  /** Whether it has settled (will not change any more). */
  done: boolean;
  peers: Record<string, OperationPeer>;
}

/** Options of waiting for a queued write to settle (`queued.applied(options)`). */
export interface WriteWaitOptions {
  /**
   * Watchdog timeout budget (ms; default WRITE_TIMEOUT_MS). Must be a finite number greater than 0.
   * Suspended while healthy and paused/holding before all target ECUs settle; scaled by slow playback speed and
   * target ECU step period while advancing towards due; bounded across disconnect/non-sync or past due.
   */
  timeoutMs?: number;
  /** Aborts the wait (`WriteError('no-answer', ..., 'aborted', lastKnownOperation)`). */
  signal?: AbortSignal;
}

/**
 * Stages 1+2 result of `client.variables.enqueue`: the server accepted the write and every target ECU has queued it
 * (or settled as `applied`). Call `applied()` for stages 3+4 (waiting for all target ECUs to apply it).
 */
export interface QueuedOperation {
  readonly session: string;
  readonly seq: number;
  applied(options?: WriteWaitOptions): Promise<OperationView>;
}

/** Options of a write. */
export interface WriteOptions {
  /** Optional server instance identifier to reject stale writes across server restarts. */
  instance?: string;
  /** Virtual time at which to apply it (ms). Omitted: the next step. */
  atMs?: number;
  /** If the requested time cannot be met: reject (default; not applied) or apply (applied in the next step). */
  late?: 'reject' | 'apply';
  /**
   * Watchdog timeout budget for waiting on the applied result (ms; default WRITE_TIMEOUT_MS). Must be a finite number
   * greater than 0. Values longer than about 24.8 days (2^31-1 ms) are capped there.
   */
  timeoutMs?: number;
  /**
   * Forwarded to the HTTP transport (`HttpRequest.keepalive` -> `fetch(..., { keepalive })`). Set `true` for a
   * release write sent from a browser `pagehide` handler so the request can finish as the page unloads.
   */
  keepalive?: boolean;
  /** Aborts the write or wait (`WriteError`). */
  signal?: AbortSignal;
}

/**
 * A write that went wrong.
 * - refused: the server did not accept it (wrong type or range, unknown variable, no run and so on; code names the
 *   error). It was not sent, so you may fix it and send it again.
 * - failed: accepted but not applied (code names the settled state; operation has the state and reason per participant).
 * - no-answer: unknown whether it was applied. The result did not come within the timeout (operation has the last
 *   known state), there was no HTTP answer, the server failed midway (500), the ECUs did not queue it in time
 *   (code `write-timeout`) or the connection broke midway (`write-failed`), the wait was aborted (`aborted`), the run
 *   changed before the answer, the write settled while reconnecting and its result was lost (not in the summary's list
 *   and older than the boundary operations_from_seq), or the server could not keep all unsettled operations and stopped
 *   following it. **Sending it again as is may write twice**: check operations and the value first.
 * - closed: the client was closed (unknown whether it was applied).
 */
export class WriteError extends Error {
  constructor(
    readonly kind: 'refused' | 'failed' | 'no-answer' | 'closed',
    message: string,
    /**
     * Name of the error (for refused: `unknown-variable`, `not-writable`, `invalid-field`, `unknown-field`,
     * `invalid-argument`, `aborted` and so on; for failed: `invalid`, `busy`, `late`, `rejected`, `cancelled`,
     * `unknown`, `mixed`; for no-answer: `write-timeout`, `write-failed`, `aborted`, or null).
     */
    readonly code: string | null = null,
    /** The operation (once its number is known). */
    readonly operation: OperationView | null = null,
  ) {
    super(message);
    this.name = 'WriteError';
  }
}

/** Read side and writes of variables (GuiClient.variables). */
export interface ClientVariables {
  /** List and state of the variables. */
  readonly state: Readable<VariablesState>;
  /**
   * Recent operations (results of writes; oldest first, up to OPERATIONS_KEPT; includes other clients' writes, not a
   * scenario's own writes).
   */
  readonly operations: Readable<readonly OperationView[]>;
  /**
   * The server refused a watch (`invalid-watch`, `too-many-watches`; null if not). Values of the refused subscription
   * do not arrive. Back to null once the watches are recreated or reduced and the subscriptions are sent again. The
   * library keeps its subscriptions within the server's limits, so this normally does not happen (it can with a server
   * that has different limits).
   */
  readonly watchProblem: Readable<WatchProblem | null>;
  /**
   * Starts receiving values of variables. `'all'` means every variable. However many watches observe the same variable,
   * the server gets one subscription per variable. Subscribes again automatically after a reconnect. Continues when the
   * run changes (values of the new run arrive).
   */
  watch(variables: readonly VariableRef[] | 'all', options?: WatchOptions): VariableWatch;
  /**
   * Sends a write and resolves with `QueuedOperation` once the server accepted it and every target ECU has queued it
   * (stages 1+2). Rejects immediately with `WriteError` if refused, unanswered, or already terminally failed.
   */
  enqueue(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<QueuedOperation>;
  /**
   * Writes variables: `write({Engine: {'engine.throttle': 50}})`. Sugar for
   * `enqueue(writes, options).then(q => q.applied(...))`: resolves with the operation once applied; rejects with a
   * `WriteError` otherwise. The same write is never resent automatically.
   */
  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<OperationView>;
  /**
   * Binds one variable to one screen widget: exposes the value state (stale, none yet, the value at the paused time and
   * so on), whether it can be written and why not, and the write status as one readable value (state), and writes with
   * write(value). This is all a slider, switch or lamp widget needs. However many places bind the same variable, there
   * is one write per variable and per run (no overlapping sends, the same status everywhere, and the status survives
   * recreating a bind). Call close() when done with it.
   */
  bind(ref: VariableRef, options?: BindOptions): VariableBinding;
}

/** Internal hook letting `FrontVariable` release its in-flight token even when `queued.applied()` is never called. */
export const ON_SETTLED = Symbol('onSettled');

export interface InternalQueuedOperation extends QueuedOperation {
  [ON_SETTLED]?(listener: (outcome: OperationView | WriteError) => void): () => void;
}

/** Clock and link signals driving the four-mode write watchdog. */
export interface WriteClockSource {
  readonly connection: Readable<ConnectionState>;
  readonly ready: Readable<boolean>;
  readonly rejected: Readable<boolean>;
  readonly incompatible: Readable<string | null>;
  readonly session: Readable<SessionView | null>;
}

/** Default time to wait for the result of a write (ms). */
export const WRITE_TIMEOUT_MS = 10_000;
/** Limit on the number of history points (per variable). */
export const HISTORY_MAX = 100_000;
/**
 * Limit on the memory taken for histories, for all variables together (in points; one point is 17 bytes). Once it is
 * reached, no variable's history grows further; each keeps its current size and drops its oldest points (so fewer
 * points than requested remain).
 */
export const HISTORY_TOTAL_MAX = 2_000_000;
/** Memory a history takes at first (in points). */
const HISTORY_FIRST = 16;
/**
 * Size limit of one watch message (bytes; the server closes the connection on a message over 256 KiB). Above it, ask
 * for "all".
 */
const WATCH_MESSAGE_BYTES = 200 * 1024;
/** Longest delay setTimeout accepts (ms). A longer value fires immediately. */
const TIMER_MAX_MS = 2 ** 31 - 1;
/** Number of recent operations kept. */
export const OPERATIONS_KEPT = 64;
/** Default watch interval (ms). */
export const WATCH_PERIOD_MS = 100;
/**
 * Intervals of the server subscriptions (ms). A requested interval is rounded to the longest of these that is not
 * longer (never slower than requested). There is one subscription per interval, so there are at most this many
 * subscriptions (the server's limit is 16 per connection).
 */
export const WATCH_PERIODS_MS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10_000, 60_000] as const;
/** Default limit on variables named in one subscription (beyond it, that subscription asks for "all"). */
const WATCH_VARIABLES = 2000;

const WRITE_OPTION_FIELDS = new Set([
  'instance',
  'atMs',
  'late',
  'timeoutMs',
  'keepalive',
  'signal',
]);
const WRITE_WAIT_OPTION_FIELDS = new Set(['timeoutMs', 'signal']);

const NO_VARIABLES: VariablesState = {
  session: null,
  status: 'unknown',
  reason: null,
  peers: [],
  availability: new Map(),
  peersOmitted: 0,
  variablesOmitted: 0,
};

/** Byte count in UTF-8. */
function utf8Length(textValue: string): number {
  let bytes = 0;
  for (let i = 0; i < textValue.length; i++) {
    const code = textValue.charCodeAt(i);
    bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : 3; // Each half of a surrogate pair counts as 3 (an overestimate)
  }
  return bytes;
}

/** Turns a variable's name into one string (neither participant names nor variable names contain NUL). */
const keyOf = (peer: string, name: string) => `${peer}\u0000${name}`;

type Fields = Record<string, unknown>;
const isObject = (value: unknown): value is Fields =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const text = (value: unknown, fallback = '') => (typeof value === 'string' ? value : fallback);
const numberOrNull = (value: unknown) =>
  typeof value === 'number' && Number.isFinite(value) ? value : null;

export function isAbortSignalLike(value: unknown): value is AbortSignal {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as AbortSignal).aborted === 'boolean' &&
    typeof (value as AbortSignal).addEventListener === 'function' &&
    typeof (value as AbortSignal).removeEventListener === 'function'
  );
}

export function validateWriteOptions(options: unknown): WriteError | null {
  if (options === undefined) return null;
  if (!isObject(options)) {
    return new WriteError('refused', 'the options of write must be an object', 'invalid-argument');
  }
  const unknown = Object.keys(options).filter((k) => !WRITE_OPTION_FIELDS.has(k));
  if (unknown.length > 0) {
    return new WriteError(
      'refused',
      `the options of write have ${unknown.join(', ')}, which write does not know: the fields are ${[...WRITE_OPTION_FIELDS].join(', ')}`,
      'unknown-field',
    );
  }
  const atMs = options['atMs'];
  if (atMs !== undefined && (typeof atMs !== 'number' || !Number.isFinite(atMs) || atMs < 0)) {
    return new WriteError(
      'refused',
      'options.atMs must be a finite number of milliseconds greater than or equal to 0',
      'invalid-argument',
    );
  }
  const late = options['late'];
  if (late !== undefined && late !== 'reject' && late !== 'apply') {
    return new WriteError(
      'refused',
      "options.late must be 'reject' or 'apply'",
      'invalid-argument',
    );
  }
  const timeoutMs = options['timeoutMs'];
  if (
    timeoutMs !== undefined &&
    (typeof timeoutMs !== 'number' || !Number.isFinite(timeoutMs) || timeoutMs <= 0)
  ) {
    return new WriteError(
      'refused',
      'options.timeoutMs must be a finite number of milliseconds greater than 0',
      'invalid-argument',
    );
  }
  const keepalive = options['keepalive'];
  if (keepalive !== undefined && typeof keepalive !== 'boolean') {
    return new WriteError('refused', 'options.keepalive must be true or false', 'invalid-argument');
  }
  const signal = options['signal'];
  if (signal !== undefined && !isAbortSignalLike(signal)) {
    return new WriteError('refused', 'options.signal must be an AbortSignal', 'invalid-argument');
  }
  return null;
}

export function validateWriteWaitOptions(options: unknown): WriteError | null {
  if (options === undefined) return null;
  if (!isObject(options)) {
    return new WriteError(
      'refused',
      'the options of applied must be an object',
      'invalid-argument',
    );
  }
  const unknown = Object.keys(options).filter((k) => !WRITE_WAIT_OPTION_FIELDS.has(k));
  if (unknown.length > 0) {
    return new WriteError(
      'refused',
      `the options of applied have ${unknown.join(', ')}, which applied does not know: the fields are ${[...WRITE_WAIT_OPTION_FIELDS].join(', ')}`,
      'unknown-field',
    );
  }
  const timeoutMs = options['timeoutMs'];
  if (
    timeoutMs !== undefined &&
    (typeof timeoutMs !== 'number' || !Number.isFinite(timeoutMs) || timeoutMs <= 0)
  ) {
    return new WriteError(
      'refused',
      'options.timeoutMs must be a finite number of milliseconds greater than 0',
      'invalid-argument',
    );
  }
  const signal = options['signal'];
  if (signal !== undefined && !isAbortSignalLike(signal)) {
    return new WriteError('refused', 'options.signal must be an AbortSignal', 'invalid-argument');
  }
  return null;
}

function failedWriteError(outcome: OperationView): WriteError {
  const reasons = Object.entries(outcome.peers)
    .filter(([, p]) => p.state !== 'applied')
    .map(([name, p]) => `${name}: ${p.state}${p.reason ? ` (${p.reason})` : ''}`);
  return new WriteError(
    'failed',
    `the write (operation ${outcome.seq}) was not applied: ${reasons.join('; ') || outcome.state}`,
    outcome.state,
    outcome,
  );
}

/** Reads `variables` of a summary. Unreadable parts are skipped as if absent (additions are compatible). */
export function readVariables(value: unknown, session: string | null): VariablesState {
  const u = new Unreadable('variables');
  const summary = lenient(
    (v) => readVariablesReport(v, 'variables', u),
    null,
    u,
  )(value, 'variables');
  if (!summary) return { ...NO_VARIABLES, session };
  const peers: VariablePeer[] = [];
  const availability = new Map<string, Map<string, VariableAvailability>>();
  for (const p of summary.peers) {
    const variables: VariableInfo[] = [];
    const states = new Map<string, VariableAvailability>();
    for (const { availability: state, ...info } of p.variables) {
      variables.push(info);
      states.set(info.name, state);
    }
    availability.set(p.name, states);
    peers.push({ name: p.name, connected: p.connected, variables });
  }
  return {
    session,
    status: summary.state,
    reason: summary.reason,
    peers,
    availability,
    peersOmitted: summary.peers_omitted,
    variablesOmitted: summary.variables_omitted,
  };
}

/** Reads an `operation` notification (or one entry of operations in a summary). null if unreadable. */
export function readOperation(value: unknown, session: string): OperationView | null {
  const u = new Unreadable('operation');
  const report = lenient((v, p) => readOperationReport(v, p, u), null, u)(value, 'operation');
  if (!report) return null;
  const peers: Record<string, OperationPeer> = {};
  for (const [name, p] of Object.entries(report.peers)) {
    peers[name] = {
      state: p.state,
      appliedAtMs: p.applied_at_ms,
      late: p.late,
      reason: p.reason,
      appliedWrites: p.applied_writes,
      failedWrite: p.failed_write,
    };
  }
  return {
    session,
    seq: report.seq,
    state: report.state,
    done: report.done,
    peers,
  };
}

/** History of one variable: a fixed-size ring buffer. */
class History {
  private times: Float64Array;
  private values: Float64Array;
  /** Kind of value: 0 number, 1 bool, 2 null. */
  private kinds: Uint8Array;
  private capacity: number;
  private start = 0;
  private length = 0;

  /** limit: the most points to keep. Memory doubles as points arrive (it is not all taken up front). */
  constructor(private max: number) {
    this.capacity = Math.min(max, HISTORY_FIRST);
    this.times = new Float64Array(this.capacity);
    this.values = new Float64Array(this.capacity);
    this.kinds = new Uint8Array(this.capacity);
  }

  get size(): number {
    return this.length;
  }

  /** Memory taken (in points). */
  get reserved(): number {
    return this.capacity;
  }

  get limit(): number {
    return this.max;
  }

  get lastTime(): number | null {
    return this.length === 0
      ? null
      : (this.times[(this.start + this.length - 1) % this.capacity] ?? null);
  }

  /**
   * Adds a point. If the buffer is full and below the limit, doubles it as far as mayGrow(number of points to add)
   * allows. If that is not allowed, keeps the current size and drops the oldest point.
   */
  push(sampleMs: number, value: VariableValue, mayGrow: (more: number) => boolean): void {
    if (this.max === 0) return;
    if (this.length === this.capacity && this.capacity < this.max) {
      const next = Math.min(this.max, this.capacity * 2);
      if (mayGrow(next - this.capacity)) this.resize(next);
    }
    const at = (this.start + this.length) % this.capacity;
    this.set(at, sampleMs, value);
    if (this.length < this.capacity) this.length += 1;
    else this.start = (this.start + 1) % this.capacity;
  }

  /** Replaces the value of the last point (when a value of the same step arrives again with a different value). */
  replaceLast(value: VariableValue): void {
    if (this.length === 0) return;
    const at = (this.start + this.length - 1) % this.capacity;
    this.set(at, this.times[at] ?? 0, value);
  }

  private set(at: number, sampleMs: number, value: VariableValue): void {
    this.times[at] = sampleMs;
    this.values[at] = value === null ? 0 : Number(value);
    this.kinds[at] = value === null ? 2 : typeof value === 'boolean' ? 1 : 0;
  }

  /** Up to n of the newest points, oldest first. */
  last(n: number): HistoryPoint[] {
    const take = Math.min(n, this.length);
    const out: HistoryPoint[] = [];
    for (let i = this.length - take; i < this.length; i++) {
      const at = (this.start + i) % this.capacity;
      const kind = this.kinds[at];
      const value = this.values[at] ?? 0;
      out.push({
        sampleMs: this.times[at] ?? 0,
        value: kind === 2 ? null : kind === 1 ? value !== 0 : value,
      });
    }
    return out;
  }

  /** Raises the limit (memory is taken as points arrive). */
  raise(max: number): void {
    if (max > this.max) this.max = max;
  }

  private resize(capacity: number): void {
    const kept = this.last(this.length);
    this.capacity = capacity;
    this.times = new Float64Array(capacity);
    this.values = new Float64Array(capacity);
    this.kinds = new Uint8Array(capacity);
    this.start = 0;
    this.length = kept.length;
    kept.forEach((point, at) => this.set(at, point.sampleMs, point.value));
  }
}

/** The inside of one watch (what the user holds). */
interface Handle {
  /** The variables watched (null: all). */
  keys: Set<string> | null;
  periodMs: number;
  history: number;
  values: Writable<WatchValues>;
}

/** One server subscription (one per interval). */
interface Bucket {
  id: number;
  periodMs: number;
  /** What was last requested from the server (JSON; not resent if unchanged). null: nothing requested. */
  asked: string | null;
}

/**
 * The operations part of a summary: the list, the boundary (null from a server that does not say), and whether the
 * server could not keep all unsettled operations.
 */
interface Listed {
  operations: readonly OperationView[];
  fromSeq: number | null;
  openOmitted: boolean;
}

/** Stage 1+2 request waiting for `POST api/write` to answer. */
interface HttpInFlight {
  session: string;
  epoch: number;
  targetPeers: readonly string[];
  requestedAtMs: number | null;
  simMsAtEnqueue: number | null;
  defaultTimeoutMs: number;
  signal: AbortSignal | undefined;
  summary?: Listed;
  resolve: (queued: InternalQueuedOperation) => void;
  reject: (error: WriteError) => void;
}

type WatchdogMode = 'armed-unhealthy' | 'suspended' | 'armed-due' | 'liveness';

/** One `queued.applied()` waiter driven by the four-mode clock-aware watchdog. */
interface WatchdogWaiter {
  tracker: QueuedTracker;
  timeoutMs: number;
  signal: AbortSignal | undefined;
  onAbort: (() => void) | null;
  mode: WatchdogMode | null;
  modeEnteredAtMs: number;
  /** Cumulative wall-clock ms spent in `armed-unhealthy`; never reset across flapping or resync. */
  unhealthyElapsedMs: number;
  /** Normalized fraction [0, 1] of active `armed-due` budget consumed; never reset on `simMs` progress. */
  dueUsed: number;
  /** Active `armed-due` budget (ms) during the previous segment, for fractional consumption across speed changes. */
  lastDueBudgetMs: number;
  /** Normalized fraction [0, 1] of active `liveness` budget consumed since last strict `simMs` progress. */
  livenessUsed: number;
  /** Active `liveness` budget (ms) during the previous segment, for fractional consumption across speed changes. */
  lastLivenessBudgetMs: number;
  /** Highest `simMs` observed so far; strict increase (`simMs > maxSimMsSeen`) resets `livenessUsed` to 0. */
  maxSimMsSeen: number | null;
  /** First `simMs` observed for an immediate write whose anchor `simMs` was initially null. */
  firstObservedSimMs: number | null;
  timer: ReturnType<typeof setTimeout> | undefined;
  resolve: (operation: OperationView) => void;
  reject: (error: WriteError) => void;
}

/**
 * Plain state tracker for a queued operation `(session, seq)`. Holds no Promise of its own until `applied()` is
 * called, so settling before `applied()` (or never calling `applied()`) never causes an unhandled rejection.
 */
interface QueuedTracker {
  session: string;
  epoch: number;
  seq: number;
  targetPeers: readonly string[];
  requestedAtMs: number | null;
  queuedHolding: boolean;
  anchorSimMs: number | null;
  defaultTimeoutMs: number;
  lastOperation: OperationView | null;
  settled: OperationView | WriteError | null;
  waiters: Set<WatchdogWaiter>;
  doneListeners: Set<(outcome: OperationView | WriteError) => void>;
}

function lost(seq: number, summary: Listed): WriteError | null {
  if (summary.fromSeq === null || seq >= summary.fromSeq) return null;
  if (summary.operations.some((o) => o.seq === seq)) return null;
  return new WriteError(
    'no-answer',
    summary.openOmitted
      ? `the server does not keep track of the write (operation ${seq}) any more (it holds too many open ` +
          'operations): its result may not come; look at the values before writing again'
      : `the result of the write (operation ${seq}) was lost while the connection was down (it was settled ` +
          'meanwhile, and the server does not list it any more): look at the values before writing again',
  );
}

function simMsOfSession(session: SessionView | null): number | null {
  if (!session) return null;
  return isHolding(session) ? 0 : simMsOf(session);
}

function controllerTimeStepMs(session: SessionView | null): number {
  const check = session?.check;
  const direct = check?.time_step_ms;
  if (typeof direct === 'number' && Number.isFinite(direct) && direct > 0) {
    return direct;
  }
  const rawStep = check && isObject(check.raw) ? check.raw['time_step_ms'] : undefined;
  if (typeof rawStep === 'number' && Number.isFinite(rawStep) && rawStep > 0) {
    return rawStep;
  }
  return 10;
}

function peerStepMs(session: SessionView | null, peerName: string, timeStepMs: number): number {
  const check = session?.check;
  const participant = check?.participants.find((p) => p.name === peerName);
  if (
    participant &&
    typeof participant.step_ms === 'number' &&
    Number.isFinite(participant.step_ms) &&
    participant.step_ms > 0
  ) {
    return participant.step_ms;
  }
  const controlRaw =
    participant?.control && isObject(participant.control.raw) ? participant.control.raw : null;
  const reported =
    participant && isObject(participant.raw) && isObject(participant.raw['reported'])
      ? participant.raw['reported']
      : null;
  const stepNs = controlRaw?.['step_ns'] ?? reported?.['step_ns'];
  if (typeof stepNs === 'number' && Number.isFinite(stepNs) && stepNs > 0) {
    return Math.max(1, Math.ceil(stepNs / 1_000_000));
  }
  return timeStepMs;
}

function isSettledPeerState(state: string | undefined): boolean {
  return state !== undefined && state !== 'sent' && state !== 'queued' && state !== 'disconnected';
}

/** What VariablesFeed asks of the connection. */
export interface VariablesLink {
  /** Sends if connected (does nothing otherwise; resume() sends what is needed on connect). */
  send(message: unknown): unknown;
  onBatch(listener: (batch: readonly unknown[], current?: () => boolean) => void): () => void;
}

const runIdOf = (value: unknown): string | null =>
  typeof value === 'string' && value.length > 0 ? value : null;

/**
 * ECU variables: list and state, value watches, writes and their results. No framework needed.
 */
export class VariablesFeed implements Omit<ClientVariables, 'bind'> {
  private instanceId: string | null = null;
  private epoch = 0;
  private lastValuesSimMs: number | null = null;
  private readonly stateValue: Writable<VariablesState>;
  private readonly operationsValue: Writable<readonly OperationView[]>;
  private readonly handles = new Set<Handle>();
  private readonly buckets = new Map<number, Bucket>();
  private readonly readings = new Map<string, VariableReading>();
  /** Cached keys kept across a reconnect until a post-reconnect `values` sample arrives. */
  private readonly awaitingRefresh = new Set<string>();
  private readonly histories = new Map<string, History>();
  private readonly inFlight = new Set<HttpInFlight>();
  private readonly trackers = new Set<QueuedTracker>();
  /** Variables someone watches (null: a watch observes all). */
  private watched: Set<string> | null = new Set();
  /** History points requested by watches of all variables (the largest request). */
  private allHistory = 0;
  /** History points requested by name (per variable, the largest among the watches that asked). */
  private namedHistory = new Map<string, number>();
  /** Total memory taken for histories (in points). */
  private historyReserved = 0;
  private readonly problemValue: Writable<WatchProblem | null>;
  private nextId = 1;
  private syncQueued = false;

  readonly state: Readable<VariablesState>;
  readonly operations: Readable<readonly OperationView[]>;
  readonly watchProblem: Readable<WatchProblem | null>;

  constructor(
    private readonly link: VariablesLink,
    private readonly http: HttpTransport,
    private readonly onError: ErrorSink = rethrowLater,
    /** Limits (from limits of api/app; the defaults until then). */
    private readonly limits: () => {
      watch_period_ms_min?: number;
      watch_period_ms_max?: number;
      watch_variables?: number;
    } = () => ({}),
    private readonly clock?: WriteClockSource,
  ) {
    this.stateValue = new Writable<VariablesState>(NO_VARIABLES, Object.is, onError);
    this.operationsValue = new Writable<readonly OperationView[]>([], Object.is, onError);
    this.problemValue = new Writable<WatchProblem | null>(null, Object.is, onError);
    this.state = this.stateValue.readonly();
    this.operations = this.operationsValue.readonly();
    this.watchProblem = this.problemValue.readonly();
    link.onBatch((batch, current) => this.receive(batch, current));
    if (clock) {
      const reevaluate = () => this.evaluateAllWaiters();
      clock.connection.subscribe(reevaluate);
      clock.ready.subscribe(reevaluate);
      clock.rejected.subscribe(reevaluate);
      clock.incompatible.subscribe(reevaluate);
      clock.session.subscribe(reevaluate);
    }
    this.stateValue.subscribe(() => this.evaluateAllWaiters());
  }

  // --- Connection -----------------------------------------------------------------------------

  /** Messages sent right after connecting (after authentication): all current subscriptions. */
  resume(): unknown[] {
    this.syncQueued = false;
    for (const key of this.readings.keys()) this.awaitingRefresh.add(key);
    const messages: unknown[] = [];
    for (const [periodMs, wanted] of this.wanted()) {
      const bucket = this.bucket(periodMs);
      const message = this.watchMessage(bucket, wanted);
      bucket.asked = JSON.stringify(message);
      messages.push(message);
    }
    for (const [periodMs, bucket] of this.buckets) {
      if (!messages.some((m) => (m as { id: number }).id === bucket.id))
        this.buckets.delete(periodMs);
    }
    return messages;
  }

  /** The client was closed: fail the pending writes (including those waiting for the HTTP answer). Watches stay. */
  dispose(): void {
    const closedError = () =>
      new WriteError('closed', 'the client was closed before the result came');
    for (const flight of [...this.inFlight]) {
      this.inFlight.delete(flight);
      flight.reject(closedError());
    }
    for (const tracker of [...this.trackers]) {
      this.settleTracker(tracker, closedError());
    }
  }

  // --- Notifications --------------------------------------------------------------------------

  onInstance(instance: string): void {
    if (this.instanceId === instance) return;
    const hadInstance = this.instanceId !== null;
    this.instanceId = instance;
    if (hadInstance) this.enter(null, true);
  }

  /**
   * Applies messages belonging to the previous session (`beforeBatch`) while `SessionFeed`
   * still holds that session, and processes connection-level resyncs/watch errors across any
   * superseded intermediate runs (`skippedBatch`) before `SessionFeed` commits the final run.
   */
  beforeRunSwitch(
    beforeBatch: readonly unknown[],
    skippedBatch: readonly unknown[],
    current: () => boolean = () => true,
  ): void {
    let sawResync = false;
    const preSwitch: unknown[] = [];
    for (const message of beforeBatch) {
      if (isObject(message) && message['type'] === 'resync') {
        sawResync = true;
      } else {
        preSwitch.push(message);
      }
    }
    if (preSwitch.length > 0) {
      this.receive(preSwitch, current);
      if (!current()) return;
    }
    for (const message of skippedBatch) {
      if (!current()) return;
      if (!isObject(message)) continue;
      const type = message['type'];
      if (type === 'resync') {
        sawResync = true;
      } else if (type === 'error') {
        const code = message['code'];
        if (code === 'invalid-watch' || code === 'too-many-watches') {
          this.problemValue.set({ code, message: text(message['message']) });
        }
      }
    }
    if (sawResync && current()) this.resyncWatches();
  }

  /**
   * Clears the previous run's observable variable state synchronously before `SessionFeed`
   * notifies `session` subscribers of a new run, returning the deferred variable notifications.
   */
  stageRunSwitch(
    instance: string | null,
    sessionId: string | null,
    instanceChanged: boolean,
  ): () => void {
    if (instance !== null) this.instanceId = instance;
    return this.prepareEnter(sessionId, instanceChanged, sessionId === null ? 'off' : 'unknown');
  }

  private receive(batch: readonly unknown[], current: () => boolean = () => true): void {
    const touched = new Set<string>();
    let touchedAll = false;
    let operations: OperationView[] | null = null;
    let fresh: OperationView[] = [];
    // Apply the operation results received so far before the run changes (so settled results that arrived in the same
    // batch do not turn into "unknown" at the switch)
    const flush = () => {
      if (operations) this.setOperations(operations, fresh, current);
      operations = null;
      fresh = [];
    };
    for (const message of batch) {
      if (!current()) return;
      if (!isObject(message)) continue;
      switch (message['type']) {
        case 'resync': {
          flush();
          if (!current()) return;
          for (const key of this.readings.keys()) this.awaitingRefresh.add(key);
          this.resyncWatches();
          touchedAll = true;
          break;
        }
        case 'hello': {
          flush();
          if (!current()) return;
          const summary = message['session'];
          const nextInstance = runIdOf(message['instance']);
          const instanceChanged =
            this.instanceId !== null && nextInstance !== null && this.instanceId !== nextInstance;
          if (nextInstance !== null) this.instanceId = nextInstance;
          if (instanceChanged) this.enter(null, true);
          if (!current()) return;
          const id = isObject(summary) ? runIdOf(summary['id']) : null;
          if (isObject(summary) && id !== null) {
            const instance = this.instanceId;
            if (id !== this.stateValue.get().session) {
              this.enter(id);
              if (!current()) return;
            }
            const sameIdentity = () =>
              current() && this.instanceId === instance && this.stateValue.get().session === id;
            const variables = summary['variables'];
            if (sameIdentity() && variables !== undefined) {
              let notifyOperations: () => void = () => undefined;
              const listed: OperationView[] = [];
              if (isObject(variables) && Array.isArray(variables['operations'])) {
                let currentOps = this.operationsValue.get();
                for (const entry of variables['operations']) {
                  const operation = readOperation(entry, id);
                  if (operation) {
                    listed.push(operation);
                    currentOps = this.withOperation(currentOps, operation);
                  }
                }
                notifyOperations = this.prepareOperations(currentOps, listed, sameIdentity);
              }
              const next = readVariables(variables, id);
              if (next.status === 'ended' || next.status === 'off' || next.status === 'failed') {
                this.awaitingRefresh.clear();
              }
              this.stateValue.set(next);
              notifyOperations();
              if (
                sameIdentity() &&
                this.stateValue.get() === next &&
                isObject(variables) &&
                Array.isArray(variables['operations'])
              ) {
                const fromSeq = variables['operations_from_seq'];
                const openOmitted = variables['operations_open_omitted'];
                this.forgetLost(id, {
                  operations: listed,
                  fromSeq: Number.isSafeInteger(fromSeq) ? (fromSeq as number) : null,
                  openOmitted: typeof openOmitted === 'number' && openOmitted > 0,
                });
              }
            }
          } else {
            this.enter(null, false, 'off');
          }
          touchedAll = true;
          break;
        }
        case 'session': {
          const summary = message['session'];
          const id = isObject(summary) ? runIdOf(summary['id']) : null;
          // Another run started: its variable notification (variables) follows. Unknown until then
          if (id !== null && id !== this.stateValue.get().session) {
            flush();
            if (!current()) return;
            this.enter(id);
            if (!current()) return;
            if (this.stateValue.get().session !== id) break;
            touchedAll = true;
          }
          break;
        }
        case 'variables': {
          const id = runIdOf(message['run_id']);
          if (id === null) break;
          const details = message['variables'];
          const existing = this.stateValue.get().session;
          if (id !== existing) {
            flush();
            if (!current()) return;
            if (this.stateValue.get().session !== id) {
              this.enter(id);
              if (!current()) return;
              if (this.stateValue.get().session !== id) break;
              touchedAll = true;
            }
          }
          const next = readVariables(details, id);
          if (next.status === 'ended' || next.status === 'off' || next.status === 'failed') {
            this.awaitingRefresh.clear();
          }
          this.stateValue.set(next);
          break;
        }
        case 'values': {
          if (runIdOf(message['run_id']) !== this.stateValue.get().session) break;
          this.takeValues(message, touched);
          break;
        }
        case 'operation': {
          const id = runIdOf(message['run_id']);
          if (id === null || id !== this.stateValue.get().session) break;
          const operation = readOperation(message, id);
          if (operation) {
            operations = this.withOperation(operations ?? this.operationsValue.get(), operation);
            fresh.push(operation);
          }
          break;
        }
        case 'availability': {
          // Replaces the states of each participant listed. A disconnection changes nothing: the last states stay
          const id = this.stateValue.get().session;
          if (runIdOf(message['run_id']) !== id || id === null) break;
          if (this.takeAvailability(message['peers'])) touchedAll = true;
          break;
        }
        case 'error': {
          // A watch was refused: on this connection, the values of that subscription do not arrive
          const code = message['code'];
          if (code === 'invalid-watch' || code === 'too-many-watches') {
            this.problemValue.set({ code, message: text(message['message']) });
          }
          break;
        }
      }
    }
    flush();
    if (current() && (touchedAll || touched.size > 0))
      this.publish(touchedAll ? null : touched, current);
  }

  /** Returns whether any state changed. Only the availability table changes: `peers` keeps its identity. */
  private takeAvailability(peers: unknown): boolean {
    if (!isObject(peers)) return false;
    const state = this.stateValue.get();
    let next: Map<string, ReadonlyMap<string, VariableAvailability>> | null = null;
    for (const peer of state.peers) {
      const told = peers[peer.name];
      if (!isObject(told)) continue;
      const before = state.availability.get(peer.name);
      const states = new Map(peer.variables.map((v) => [v.name, readAvailability(told[v.name])]));
      if (
        before?.size === states.size &&
        [...states].every(([name, value]) => before.get(name) === value)
      )
        continue;
      next ??= new Map(state.availability);
      next.set(peer.name, states);
    }
    if (!next) return false;
    this.stateValue.set({ ...state, availability: next });
    return true;
  }

  /**
   * With the summary after a reconnect, settles as "no answer came" (lost) the writes waiting for
   * a result that will no longer arrive. For writes whose number is not known yet (before the HTTP answer), the summary
   * is kept and checked the same way once the number is known.
   */
  private forgetLost(session: string, summary: Listed): void {
    for (const flight of [...this.inFlight]) {
      if (flight.session === session) flight.summary = summary;
    }
    for (const tracker of [...this.trackers]) {
      if (tracker.session !== session) continue;
      const error = lost(tracker.seq, summary);
      if (error) this.settleTracker(tracker, error);
    }
  }

  /**
   * Switched to another run: drop the previous run's values, histories and operations. Writes waiting for the previous
   * run's result are failed, since the result will not come.
   */
  private enter(
    session: string | null,
    force = false,
    status: VariablesStatus = 'unknown',
  ): VariablesState {
    this.prepareEnter(session, force, status)();
    return this.stateValue.get();
  }

  private prepareEnter(
    session: string | null,
    force = false,
    status: VariablesStatus = 'unknown',
  ): () => void {
    const current = this.stateValue.get();
    if (!force && session === current.session && status === current.status) {
      return () => undefined;
    }
    // Clear every observable part before notifying any subscriber. A subscriber can synchronously
    // fetch the new run, so it must never observe or revive the previous run's values or schema.
    this.epoch += 1;
    this.lastValuesSimMs = null;
    this.readings.clear();
    this.awaitingRefresh.clear();
    this.histories.clear();
    this.historyReserved = 0;
    const initial = { ...NO_VARIABLES, session, status };
    const notifyOperations = this.operationsValue.setDeferred([]);
    const notifyState = this.stateValue.setDeferred(initial);
    const notifyWatches = [...this.handles].map((handle) => handle.values.setDeferred({}));
    for (const flight of [...this.inFlight]) {
      if (force || flight.session !== session) {
        this.inFlight.delete(flight);
        flight.reject(
          new WriteError(
            'no-answer',
            `the run ${flight.session} is over (another one has started): the result of the write did not come`,
          ),
        );
      }
    }
    for (const tracker of [...this.trackers]) {
      if (force || tracker.session !== session) {
        this.settleTracker(
          tracker,
          new WriteError(
            'no-answer',
            `the run ${tracker.session} is over (another one has started): the result of the write ` +
              `(operation ${tracker.seq}) did not come`,
          ),
        );
      }
    }
    return () => {
      notifyOperations();
      notifyState();
      for (const notify of notifyWatches) notify();
    };
  }

  private takeValues(message: Fields, touched: Set<string>): void {
    const peers = message['peers'];
    if (!isObject(peers)) return;
    const simMs = numberOrNull(message['sim_ms']);
    if (simMs !== null && (this.lastValuesSimMs === null || simMs > this.lastValuesSimMs)) {
      this.lastValuesSimMs = simMs;
      this.evaluateAllWaiters();
    }
    const paused = message['paused'] === true;
    const mayGrow = (more: number) => {
      if (this.historyReserved + more > HISTORY_TOTAL_MAX) return false;
      this.historyReserved += more;
      return true;
    };
    for (const [peer, entry] of Object.entries(peers)) {
      if (!isObject(entry) || !isObject(entry['values'])) continue;
      const sampleMs = numberOrNull(entry['sample_ms']) ?? 0;
      for (const [name, raw] of Object.entries(entry['values'])) {
        const value: VariableValue =
          typeof raw === 'boolean' || (typeof raw === 'number' && Number.isFinite(raw))
            ? raw
            : null;
        const key = keyOf(peer, name);
        // Do not keep variables nobody watches (values arriving right after a stop, extras of an "all" subscription)
        if (this.watched !== null && !this.watched.has(key)) continue;
        const refreshed = this.awaitingRefresh.delete(key);
        const before = this.readings.get(key);
        if (
          before &&
          before.sampleMs === sampleMs &&
          before.simMs === simMs &&
          before.paused === paused &&
          Object.is(before.value, value)
        ) {
          if (refreshed) touched.add(key);
          continue;
        }
        this.readings.set(key, { value, sampleMs, simMs, paused });
        touched.add(key);
        // History: up to the number requested (the larger of the by-name watches and the all-variables watches). Memory
        // is taken as points arrive
        const wanted = Math.max(this.namedHistory.get(key) ?? 0, this.allHistory);
        if (wanted === 0) continue;
        let history = this.histories.get(key);
        if (!history) {
          if (!mayGrow(Math.min(wanted, HISTORY_FIRST))) continue;
          history = new History(wanted);
          this.histories.set(key, history);
        } else {
          history.raise(wanted);
        }
        const last = history.lastTime;
        if (last === null || sampleMs > last) history.push(sampleMs, value, mayGrow);
        else if (sampleMs === last) history.replaceLast(value); // A value of the same step arrived again, changed
      }
    }
  }

  /** Rebuilds values of the watches that observe the changed variables (all watches if touched is null). */
  private publish(touched: Set<string> | null, current: () => boolean = () => true): void {
    for (const handle of this.handles) {
      if (!current()) return;
      if (touched !== null && handle.keys !== null) {
        let hit = false;
        for (const key of handle.keys) {
          if (touched.has(key)) {
            hit = true;
            break;
          }
        }
        if (!hit) continue;
      }
      handle.values.set(this.valuesOf(handle));
    }
  }

  private valuesOf(handle: Handle): WatchValues {
    const out: Record<string, Record<string, VariableReading>> = {};
    const add = (key: string, reading: VariableReading) => {
      const cut = key.indexOf('\u0000');
      (out[key.slice(0, cut)] ??= {})[key.slice(cut + 1)] = reading;
    };
    if (handle.keys === null) {
      for (const [key, reading] of this.readings) add(key, reading);
    } else {
      for (const key of handle.keys) {
        const reading = this.readings.get(key);
        if (reading) add(key, reading);
      }
    }
    return out;
  }

  // --- Operations & Four-Stage Write -----------------------------------------------------------

  private findOperation(session: string, seq: number): OperationView | undefined {
    return this.operationsValue.get().find((o) => o.seq === seq && o.session === session);
  }

  private withOperation(
    operations: readonly OperationView[],
    operation: OperationView,
  ): OperationView[] {
    const at = operations.findIndex((o) => o.seq === operation.seq);
    // A settled operation never changes again (an old state arriving late does not set it back)
    if (at >= 0 && operations[at]?.done) return [...operations];
    const next =
      at >= 0 ? operations.map((o, i) => (i === at ? operation : o)) : [...operations, operation];
    return next.length > OPERATIONS_KEPT ? next.slice(next.length - OPERATIONS_KEPT) : next;
  }

  private setOperations(
    operations: readonly OperationView[],
    fresh: readonly OperationView[] = [],
    current: () => boolean = () => true,
  ): void {
    this.prepareOperations(operations, fresh, current)();
  }

  private prepareOperations(
    operations: readonly OperationView[],
    fresh: readonly OperationView[],
    current: () => boolean,
  ): () => void {
    if (!current()) return () => undefined;
    // Install the observable list and settle confirmed results before any subscriber runs.
    const notify = this.operationsValue.setDeferred(operations);
    for (const tracker of [...this.trackers]) {
      let latest: OperationView | undefined;
      for (let i = fresh.length - 1; i >= 0; i--) {
        const candidate = fresh[i];
        if (candidate && candidate.seq === tracker.seq && candidate.session === tracker.session) {
          latest = candidate;
          break;
        }
      }
      if (!latest) {
        latest = operations.find((o) => o.seq === tracker.seq && o.session === tracker.session);
      }
      if (latest) {
        tracker.lastOperation = latest;
        if (latest.done) {
          this.settleTracker(tracker, latest);
        }
      }
    }
    this.evaluateAllWaiters();
    return notify;
  }

  private settleTracker(tracker: QueuedTracker, outcome: OperationView | WriteError): void {
    if (tracker.settled !== null) return;
    tracker.settled = outcome;
    if (!(outcome instanceof WriteError)) {
      tracker.lastOperation = outcome;
    }
    this.trackers.delete(tracker);
    for (const listener of [...tracker.doneListeners]) {
      tracker.doneListeners.delete(listener);
      try {
        listener(outcome);
      } catch (error) {
        this.onError(error);
      }
    }
    for (const waiter of [...tracker.waiters]) {
      this.finishWaiter(waiter, outcome);
    }
  }

  private cleanupWaiter(waiter: WatchdogWaiter): void {
    clearTimeout(waiter.timer);
    waiter.timer = undefined;
    if (waiter.onAbort && waiter.signal) {
      waiter.signal.removeEventListener('abort', waiter.onAbort);
      waiter.onAbort = null;
    }
    waiter.tracker.waiters.delete(waiter);
  }

  private finishWaiter(waiter: WatchdogWaiter, outcome: OperationView | WriteError): void {
    if (!waiter.tracker.waiters.has(waiter)) return;
    this.cleanupWaiter(waiter);
    if (outcome instanceof WriteError) {
      waiter.reject(outcome);
    } else if (outcome.state === 'applied') {
      waiter.resolve(outcome);
    } else {
      waiter.reject(failedWriteError(outcome));
    }
  }

  private currentSimMs(session: string): number | null {
    const sessionView = this.clock?.session.get() ?? null;
    const fromSession =
      sessionView && sessionView.id === session ? simMsOfSession(sessionView) : null;
    const fromValues = this.stateValue.get().session === session ? this.lastValuesSimMs : null;
    if (fromSession !== null && fromValues !== null) return Math.max(fromSession, fromValues);
    return fromSession ?? fromValues;
  }

  private evaluateAllWaiters(): void {
    if (this.trackers.size === 0) return;
    for (const tracker of [...this.trackers]) {
      for (const waiter of [...tracker.waiters]) {
        this.evaluateWaiter(waiter);
      }
    }
  }

  private evaluateWaiter(waiter: WatchdogWaiter): void {
    const { tracker } = waiter;
    if (!tracker.waiters.has(waiter) || tracker.settled !== null) return;

    const now = Date.now();
    const dt = Math.max(0, now - waiter.modeEnteredAtMs);
    waiter.modeEnteredAtMs = now;

    if (dt > 0) {
      if (waiter.mode === 'armed-unhealthy') {
        waiter.unhealthyElapsedMs += dt;
      } else if (waiter.mode === 'armed-due' && waiter.lastDueBudgetMs > 0) {
        waiter.dueUsed = Math.min(1, waiter.dueUsed + dt / waiter.lastDueBudgetMs);
      } else if (waiter.mode === 'liveness' && waiter.lastLivenessBudgetMs > 0) {
        waiter.livenessUsed = Math.min(1, waiter.livenessUsed + dt / waiter.lastLivenessBudgetMs);
      }
    }

    const sessionView = this.clock?.session.get() ?? null;
    const varsState = this.stateValue.get();
    const simMs = this.currentSimMs(tracker.session);
    if (simMs !== null) {
      if (waiter.firstObservedSimMs === null) waiter.firstObservedSimMs = simMs;
      if (waiter.maxSimMsSeen === null) {
        waiter.maxSimMsSeen = simMs;
      } else if (simMs > waiter.maxSimMsSeen) {
        waiter.maxSimMsSeen = simMs;
        waiter.livenessUsed = 0;
      }
    }

    const lastOp =
      tracker.lastOperation ?? this.findOperation(tracker.session, tracker.seq) ?? null;
    if (lastOp) tracker.lastOperation = lastOp;
    const incompletePeers =
      tracker.targetPeers.length > 0
        ? tracker.targetPeers.filter((p) => !isSettledPeerState(lastOp?.peers[p]?.state))
        : [];

    const timeStepMs = controllerTimeStepMs(sessionView);
    let targetStepMs = timeStepMs;
    let dueSimMs: number | null = null;
    const anchorSimMs = tracker.anchorSimMs ?? waiter.firstObservedSimMs;
    const hasAnchor =
      tracker.requestedAtMs !== null || tracker.queuedHolding || anchorSimMs !== null;
    const duePeers = incompletePeers.length > 0 ? incompletePeers : tracker.targetPeers;
    for (const peerName of duePeers) {
      const stepMs = peerStepMs(sessionView, peerName, timeStepMs);
      if (incompletePeers.length === 0 || incompletePeers.includes(peerName)) {
        if (stepMs > targetStepMs) targetStepMs = stepMs;
      }
      if (hasAnchor) {
        const earliestStep = tracker.queuedHolding
          ? 0
          : anchorSimMs !== null
            ? (Math.floor(anchorSimMs / stepMs) + 1) * stepMs
            : 0;
        const atStep =
          tracker.requestedAtMs !== null ? Math.ceil(tracker.requestedAtMs / stepMs) * stepMs : 0;
        const dueStart = Math.max(atStep, earliestStep);
        const duePass = dueStart + Math.max(stepMs, timeStepMs);
        if (dueSimMs === null || duePass > dueSimMs) {
          dueSimMs = duePass;
        }
      }
    }

    let nextMode: WatchdogMode;
    if (!this.clock) {
      nextMode = 'liveness';
    } else {
      const allIncompleteConnected = incompletePeers.every((peerName) => {
        const vp = varsState.peers.find((p) => p.name === peerName);
        if (!vp || !vp.connected) return false;
        if (lastOp?.peers[peerName]?.state === 'disconnected') return false;
        const sp = sessionView?.participants.find((p) => p.name === peerName);
        if (
          sp &&
          (sp.state === 'ended' ||
            sp.state === 'failed' ||
            sp.state === 'killed' ||
            sp.state === 'not_started' ||
            (sp.controlLink !== null && sp.controlLink.state !== 'connected'))
        ) {
          return false;
        }
        return true;
      });
      const healthy =
        this.clock.connection.get() === 'open' &&
        this.clock.ready.get() === true &&
        !this.clock.rejected.get() &&
        this.clock.incompatible.get() === null &&
        sessionView !== null &&
        sessionView.id === tracker.session &&
        isRunActive(sessionView) &&
        sessionView.result === null &&
        sessionView.launcher.silentSeconds === null &&
        varsState.session === tracker.session &&
        varsState.status === 'ready' &&
        allIncompleteConnected;

      if (!healthy) {
        nextMode = 'armed-unhealthy';
      } else if (
        (isHolding(sessionView) || standsAt(sessionView) !== null) &&
        !onItsWay(sessionView)
      ) {
        nextMode = 'suspended';
      } else if (dueSimMs !== null && simMs !== null && simMs >= dueSimMs) {
        nextMode = 'armed-due';
      } else {
        nextMode = 'liveness';
      }
    }

    clearTimeout(waiter.timer);
    waiter.timer = undefined;
    waiter.mode = nextMode;

    if (nextMode === 'suspended') {
      return;
    }

    const rawSpeed = sessionView?.launcherState?.speed ?? sessionView?.speed ?? null;
    const effectiveMs =
      typeof rawSpeed === 'number' && Number.isFinite(rawSpeed) && rawSpeed > 0 && rawSpeed < 1
        ? Math.min(
            TIMER_MAX_MS,
            Math.max(waiter.timeoutMs, Math.ceil((targetStepMs / rawSpeed) * 2) + 2000),
          )
        : waiter.timeoutMs;

    let rawRemainingMs: number;
    if (nextMode === 'armed-unhealthy') {
      rawRemainingMs = waiter.timeoutMs - waiter.unhealthyElapsedMs;
    } else if (nextMode === 'armed-due') {
      waiter.lastDueBudgetMs = effectiveMs;
      rawRemainingMs = (1 - waiter.dueUsed) * effectiveMs;
    } else {
      waiter.lastLivenessBudgetMs = effectiveMs;
      rawRemainingMs = (1 - waiter.livenessUsed) * effectiveMs;
    }
    const remainingMs =
      Math.abs(rawRemainingMs - Math.round(rawRemainingMs)) < 1e-6
        ? Math.round(rawRemainingMs)
        : rawRemainingMs;

    if (remainingMs <= 0) {
      const last =
        tracker.lastOperation ?? this.findOperation(tracker.session, tracker.seq) ?? null;
      this.finishWaiter(
        waiter,
        new WriteError(
          'no-answer',
          `no result of the write (operation ${tracker.seq}) came in ${waiter.timeoutMs / 1000} s` +
            (last
              ? `: it is ${last.state} (it is applied when the simulation reaches its step)`
              : ''),
          null,
          last,
        ),
      );
      return;
    }

    const delayMs = Math.min(TIMER_MAX_MS, Math.max(1, Math.ceil(remainingMs)));
    waiter.timer = setTimeout(() => {
      this.evaluateWaiter(waiter);
    }, delayMs);
    (waiter.timer as { unref?: () => void }).unref?.();
  }

  private waitForApplied(
    tracker: QueuedTracker,
    options?: WriteWaitOptions,
  ): Promise<OperationView> {
    const bad = validateWriteWaitOptions(options);
    if (bad) return Promise.reject(bad);
    const askedTimeout = options?.timeoutMs;
    const timeoutMs = Math.min(
      askedTimeout !== undefined ? askedTimeout : tracker.defaultTimeoutMs,
      TIMER_MAX_MS,
    );
    const signal = options?.signal;
    if (signal?.aborted) {
      const last =
        (tracker.settled && !(tracker.settled instanceof WriteError) ? tracker.settled : null) ??
        tracker.lastOperation ??
        this.findOperation(tracker.session, tracker.seq) ??
        null;
      return Promise.reject(
        new WriteError(
          'no-answer',
          `the wait for the write (operation ${tracker.seq}) was aborted: it may still take effect`,
          'aborted',
          last,
        ),
      );
    }
    if (tracker.settled !== null) {
      if (tracker.settled instanceof WriteError) return Promise.reject(tracker.settled);
      if (tracker.settled.state === 'applied') return Promise.resolve(tracker.settled);
      return Promise.reject(failedWriteError(tracker.settled));
    }
    return new Promise<OperationView>((resolve, reject) => {
      const initialSim = this.currentSimMs(tracker.session);
      const waiter: WatchdogWaiter = {
        tracker,
        timeoutMs,
        signal,
        onAbort: null,
        mode: null,
        modeEnteredAtMs: Date.now(),
        unhealthyElapsedMs: 0,
        dueUsed: 0,
        lastDueBudgetMs: timeoutMs,
        livenessUsed: 0,
        lastLivenessBudgetMs: timeoutMs,
        maxSimMsSeen: initialSim,
        firstObservedSimMs: initialSim,
        timer: undefined,
        resolve,
        reject,
      };
      if (signal) {
        const onAbort = () => {
          const last =
            tracker.lastOperation ?? this.findOperation(tracker.session, tracker.seq) ?? null;
          this.finishWaiter(
            waiter,
            new WriteError(
              'no-answer',
              `the wait for the write (operation ${tracker.seq}) was aborted: it may still take effect`,
              'aborted',
              last,
            ),
          );
        };
        waiter.onAbort = onAbort;
        signal.addEventListener('abort', onAbort, { once: true });
      }
      tracker.waiters.add(waiter);
      this.evaluateWaiter(waiter);
    });
  }

  enqueue(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<QueuedOperation> {
    return this.enqueueInternal(writes, options);
  }

  /**
   * Internal path for `VariableBinder`: invokes `onQueued(seq)` synchronously when the HTTP 200 answer arrives so
   * synchronous widget state (`BoundVariable.writeStatus` / held-value dispatch) transitions in the same turn, then
   * waits on the same four-mode clock-aware watchdog (`waitForApplied`).
   */
  enqueueForBinder(
    writes: Record<string, Record<string, number | boolean | string>>,
    onQueued: (seq: number) => void,
  ): Promise<OperationView> {
    return this.enqueueInternal(writes, undefined, onQueued).then((queued) => queued.applied());
  }

  private enqueueInternal(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
    onQueuedSync?: (seq: number) => void,
  ): Promise<QueuedOperation> {
    const bad = validateWriteOptions(options);
    if (bad) return Promise.reject(bad);
    if (options?.signal?.aborted) {
      return Promise.reject(
        new WriteError('refused', 'the write was aborted before it was sent', 'aborted'),
      );
    }
    const defaultTimeoutMs = Math.min(options?.timeoutMs ?? WRITE_TIMEOUT_MS, TIMER_MAX_MS);
    const session = this.stateValue.get().session;
    const epoch = this.epoch;
    if (session === null) {
      return Promise.reject(
        new WriteError('refused', 'there is no run to write to', 'not-running'),
      );
    }
    const simMsAtEnqueue = this.currentSimMs(session);
    const targetPeers = isObject(writes) ? Object.keys(writes) : [];

    const body: Fields = { run_id: session, writes };
    const instance = options?.instance ?? this.instanceId ?? undefined;
    if (instance !== undefined) body['instance'] = instance;
    if (options?.atMs !== undefined) body['at_ms'] = options.atMs;
    if (options?.late !== undefined) body['late'] = options.late;

    return new Promise<QueuedOperation>((resolve, reject) => {
      const flight: HttpInFlight = {
        session,
        epoch,
        targetPeers,
        requestedAtMs: options?.atMs ?? null,
        simMsAtEnqueue,
        defaultTimeoutMs,
        signal: options?.signal,
        resolve,
        reject,
      };
      this.inFlight.add(flight);
      const request: HttpRequest = { method: 'POST', path: 'api/write', body };
      if (options?.keepalive !== undefined) request.keepalive = options.keepalive;
      if (options?.signal !== undefined) request.signal = options.signal;
      this.http(request, (outcome) => {
        if (!this.inFlight.delete(flight)) return; // close() or run switch failed it first
        if (!outcome.ok) {
          const error = outcome.error;
          if (flight.signal?.aborted && error instanceof HttpError && error.status === 0) {
            reject(
              new WriteError(
                'no-answer',
                'the write was aborted while waiting for the server: it may have been taken; look at the operations and the values before writing again',
                'aborted',
              ),
            );
            return;
          }
          const code = error instanceof HttpError ? (error.problem?.code ?? null) : null;
          const unsure = error instanceof HttpError && [0, 500, 502, 504].includes(error.status);
          const detailSeq = error instanceof HttpError ? error.problem?.operation : undefined;
          const partialOp: OperationView | null =
            detailSeq !== undefined
              ? (this.findOperation(flight.session, detailSeq) ?? {
                  session: flight.session,
                  seq: detailSeq,
                  state: error.problem?.state ?? 'sent',
                  done: false,
                  peers: {},
                })
              : null;
          reject(
            new WriteError(
              unsure ? 'no-answer' : 'refused',
              unsure
                ? `${errorMessage(error)} (the write may have been taken: look at the operations ` +
                    'and the values before writing again)'
                : errorMessage(error),
              code,
              partialOp,
            ),
          );
          return;
        }
        const answer = outcome.body;
        const seq = isObject(answer) ? answer['operation'] : null;
        const answered = isObject(answer) ? runIdOf(answer['run_id']) : null;
        if (!Number.isSafeInteger(seq) || answered === null) {
          reject(
            new WriteError(
              'no-answer',
              'the server answered the write without an operation number',
            ),
          );
          return;
        }
        if (
          answered !== flight.session ||
          this.stateValue.get().session !== flight.session ||
          this.epoch !== flight.epoch
        ) {
          reject(
            new WriteError(
              'no-answer',
              `the run ${flight.session} ended before the answer to the write came (operation ${String(seq)}): ` +
                'its result will not come; look at the values before writing again',
            ),
          );
          return;
        }
        const opSeq = seq as number;
        const summary = flight.summary;
        const already =
          this.findOperation(flight.session, opSeq) ??
          summary?.operations.find((o) => o.seq === opSeq && o.session === flight.session);
        if (already?.done && already.state !== 'applied') {
          reject(failedWriteError(already));
          return;
        }
        const lostErr = !already?.done && summary ? lost(opSeq, summary) : null;
        const initialSettled: OperationView | WriteError | null = already?.done ? already : lostErr;
        const activeSessionView = this.clock?.session.get() ?? null;
        const sameSessionView =
          activeSessionView && activeSessionView.id === flight.session ? activeSessionView : null;
        const queuedHolding = isHolding(sameSessionView);
        const anchorSimMs = this.currentSimMs(flight.session) ?? flight.simMsAtEnqueue;
        const tracker: QueuedTracker = {
          session: flight.session,
          epoch: flight.epoch,
          seq: opSeq,
          targetPeers: flight.targetPeers,
          requestedAtMs: flight.requestedAtMs,
          queuedHolding,
          anchorSimMs,
          defaultTimeoutMs: flight.defaultTimeoutMs,
          lastOperation: already ?? null,
          settled: initialSettled,
          waiters: new Set(),
          doneListeners: new Set(),
        };
        if (initialSettled === null) {
          this.trackers.add(tracker);
        }
        if (onQueuedSync) {
          try {
            onQueuedSync(opSeq);
          } catch (error) {
            this.onError(error);
          }
        }
        const queued: InternalQueuedOperation = {
          session: tracker.session,
          seq: tracker.seq,
          applied: (waitOptions?: WriteWaitOptions) => this.waitForApplied(tracker, waitOptions),
          [ON_SETTLED]: (listener: (outcome: OperationView | WriteError) => void) => {
            if (tracker.settled !== null) {
              listener(tracker.settled);
              return () => undefined;
            }
            tracker.doneListeners.add(listener);
            return () => {
              tracker.doneListeners.delete(listener);
            };
          },
        };
        resolve(queued);
      });
    });
  }

  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<OperationView> {
    return this.enqueue(writes, options).then((queued) =>
      queued.applied({
        ...(options?.timeoutMs !== undefined ? { timeoutMs: options.timeoutMs } : {}),
        ...(options?.signal !== undefined ? { signal: options.signal } : {}),
      }),
    );
  }

  // --- watch ----------------------------------------------------------------------------------

  watch(variables: readonly VariableRef[] | 'all', options: WatchOptions = {}): VariableWatch {
    const history = options.history ?? 0;
    const handle: Handle = {
      keys: variables === 'all' ? null : new Set(variables.map((v) => keyOf(v.peer, v.name))),
      periodMs: this.period(options.periodMs ?? WATCH_PERIOD_MS),
      // A non-number or a negative value means 0 (keep none)
      history: Number.isFinite(history)
        ? Math.max(0, Math.min(Math.floor(history), HISTORY_MAX))
        : 0,
      values: new Writable<WatchValues>({}, Object.is, this.onError),
    };
    this.handles.add(handle);
    this.forget();
    handle.values.set(this.valuesOf(handle));
    this.queueSync();
    let open = true;
    const mine = (ref: VariableRef) => {
      const key = keyOf(ref.peer, ref.name);
      return open && (handle.keys === null || handle.keys.has(key)) ? key : null;
    };
    return {
      values: handle.values.readonly(),
      value: (ref) => {
        const key = mine(ref);
        return key === null ? undefined : this.readings.get(key);
      },
      history: (ref) => {
        const key = mine(ref);
        return key === null ? [] : (this.histories.get(key)?.last(handle.history) ?? []);
      },
      close: () => {
        if (!open) return;
        open = false;
        this.handles.delete(handle);
        this.forget();
        this.queueSync();
      },
    };
  }

  isStale(ref: VariableRef): boolean {
    return this.awaitingRefresh.has(keyOf(ref.peer, ref.name));
  }

  runEpoch(): number {
    return this.epoch;
  }

  /**
   * Turns a requested interval into the longest of WATCH_PERIODS_MS that is not longer, kept within the server's range
   * (lower and upper limit).
   */
  private period(wanted: number): number {
    const limits = this.limits();
    const min = limits.watch_period_ms_min ?? WATCH_PERIODS_MS[0];
    const max = limits.watch_period_ms_max ?? WATCH_PERIODS_MS[WATCH_PERIODS_MS.length - 1] ?? min;
    let chosen: number = WATCH_PERIODS_MS[0];
    for (const period of WATCH_PERIODS_MS) if (period <= wanted) chosen = period;
    return Math.min(Math.max(chosen, min), Math.max(max, min));
  }

  /** Recounts who watches what, and drops the values of variables nobody watches and histories nobody requests. */
  private forget(): void {
    let all = false;
    let allHistory = 0;
    const seen = new Set<string>();
    const named = new Map<string, number>();
    for (const handle of this.handles) {
      if (handle.keys === null) {
        all = true;
        allHistory = Math.max(allHistory, handle.history);
        continue;
      }
      for (const key of handle.keys) {
        seen.add(key);
        if (handle.history > 0) named.set(key, Math.max(named.get(key) ?? 0, handle.history));
      }
    }
    this.watched = all ? null : seen;
    this.allHistory = allHistory;
    this.namedHistory = named;
    for (const [key, history] of this.histories) {
      if (allHistory === 0 && !named.has(key)) {
        this.historyReserved -= history.reserved;
        this.histories.delete(key);
      }
    }
    if (all) return;
    for (const key of this.readings.keys()) {
      if (!seen.has(key)) {
        this.readings.delete(key);
        this.awaitingRefresh.delete(key);
      }
    }
  }

  /**
   * Per interval, the variables to request (null: all). A variable goes to the shortest interval among the watches that
   * observe it.
   */
  private wanted(): Map<number, Set<string> | null> {
    const fastest = new Map<string, number>();
    let allPeriod: number | null = null;
    for (const handle of this.handles) {
      if (handle.keys === null) {
        allPeriod = Math.min(allPeriod ?? Infinity, handle.periodMs);
        continue;
      }
      for (const key of handle.keys) {
        fastest.set(key, Math.min(fastest.get(key) ?? Infinity, handle.periodMs));
      }
    }
    const out = new Map<number, Set<string> | null>();
    if (allPeriod !== null) out.set(allPeriod, null);
    const limit = this.limits().watch_variables ?? WATCH_VARIABLES;
    for (const [key, periodMs] of fastest) {
      // If an "all" subscription has the same or a shorter interval, the values arrive there
      if (allPeriod !== null && allPeriod <= periodMs) continue;
      const keys = out.get(periodMs);
      if (keys === undefined) out.set(periodMs, new Set([key]));
      else keys?.add(key);
    }
    // Beyond the number that can be named, that interval asks for all (and uses only the watched variables on receipt)
    for (const [periodMs, keys] of out) if (keys && keys.size > limit) out.set(periodMs, null);
    return out;
  }

  private bucket(periodMs: number): Bucket {
    let bucket = this.buckets.get(periodMs);
    if (!bucket) {
      bucket = { id: this.nextId++, periodMs, asked: null };
      this.buckets.set(periodMs, bucket);
    }
    return bucket;
  }

  private watchMessage(bucket: Bucket, wanted: Set<string> | null): unknown {
    const message = (variables: unknown) => ({
      type: 'watch',
      id: bucket.id,
      variables,
      period_ms: bucket.periodMs,
    });
    if (wanted === null) return message('all');
    const named = message(
      [...wanted].sort().map((key) => {
        const cut = key.indexOf('\u0000');
        return { peer: key.slice(0, cut), name: key.slice(cut + 1) };
      }),
    );
    return utf8Length(JSON.stringify(named)) > WATCH_MESSAGE_BYTES ? message('all') : named;
  }

  /** After watches were added or removed, brings the server subscriptions in line, once for all changes. */
  private queueSync(): void {
    if (this.syncQueued) return;
    this.syncQueued = true;
    queueMicrotask(() => {
      if (!this.syncQueued) return;
      this.syncQueued = false;
      this.sync();
    });
  }

  private resyncWatches(): void {
    this.syncQueued = false;
    const wanted = this.wanted();
    for (const [periodMs, bucket] of [...this.buckets]) {
      if (!wanted.has(periodMs)) {
        this.buckets.delete(periodMs);
        if (bucket.asked !== null) this.link.send({ type: 'unwatch', id: bucket.id });
      } else {
        bucket.asked = null;
      }
    }
    this.sync();
  }

  private sync(): void {
    const wanted = this.wanted();
    for (const [periodMs, bucket] of [...this.buckets]) {
      if (wanted.has(periodMs)) continue;
      this.buckets.delete(periodMs);
      if (bucket.asked !== null) this.link.send({ type: 'unwatch', id: bucket.id });
    }
    for (const [periodMs, keys] of wanted) {
      const bucket = this.bucket(periodMs);
      const message = this.watchMessage(bucket, keys);
      const asked = JSON.stringify(message);
      if (asked === bucket.asked) continue;
      bucket.asked = asked;
      this.problemValue.set(null); // Requested again: the earlier refusal no longer applies
      this.link.send(message);
    }
  }
}
