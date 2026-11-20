import { HttpError, HttpTransport, errorMessage } from './http';
import { Open, readAvailability } from './protocol';
import type { VariableAvailability } from './protocol';
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
  session: number | null;
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
  session: number;
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

/** Options of a write. */
export interface WriteOptions {
  /** Virtual time at which to apply it (ms). Omitted: the next step. */
  atMs?: number;
  /** If the requested time cannot be met: reject (default; not applied) or apply (applied in the next step). */
  late?: 'reject' | 'apply';
  /**
   * How long to wait for the result (whether it was applied) (ms; default WRITE_TIMEOUT_MS). While paused, and for a
   * write at a later time (atMs), the write does not settle until the simulation gets there, so make this longer or
   * follow the result in operations. `Infinity` never times out (waits until the run changes or close()). Values
   * longer than about 24.8 days (2^31-1 ms) are capped there. A non-number or a value of 0 or less means the default.
   * Counted from the HTTP answer.
   */
  timeoutMs?: number;
  /**
   * Called once when the server receives the write and returns the operation number (before the result; not called
   * when refused or when no answer came). The number lets you follow the in-progress states (queued and so on) in
   * `operations`, so a widget can show "accepted, waiting to be applied" or know that it may send the next write.
   */
  onAccepted?: (operation: { session: number; seq: number }) => void;
}

/**
 * A write that went wrong.
 * - refused: the server did not accept it (wrong type or range, unknown variable, no run and so on; code names the
 *   error). It was not sent, so you may fix it and send it again.
 * - failed: accepted but not applied (operation has the state and reason per participant).
 * - no-answer: unknown whether it was applied. The result did not come within the timeout (operation has the last
 *   known state), there was no HTTP answer, the server failed midway (500), the Control Server's answer could not be
 *   waited for (code `write-timeout`) or the connection broke midway (`write-failed`), the run changed before the
 *   answer, the write settled while reconnecting and its result was lost (not in the summary's list and older than
 *   the boundary operations_from_seq), or the server could not keep all unsettled operations and stopped following it.
 *   **Sending it again as is may write twice**: check operations and the value first.
 * - closed: the client was closed (unknown whether it was applied).
 */
export class WriteError extends Error {
  constructor(
    readonly kind: 'refused' | 'failed' | 'no-answer' | 'closed',
    message: string,
    /**
     * Name of the server's error (for refused: `unknown-variable`, `not-writable`, `invalid-field` and so on; for
     * no-answer: `write-timeout`, `write-failed`; null unless it is an HTTP error).
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
   * Writes variables: `write({Engine: {'engine.throttle': 50}})`. Sent to the current run (session of state); the
   * server refuses if that run is not running (refused, code session-changed / not-running; if the run is not known
   * yet, nothing is sent and the error is not-running). For an enum, give the value (number) or the item's name.
   * Resolves with the operation once applied. Otherwise rejects with a WriteError (tell the cases apart by kind). The
   * same write is never resent automatically.
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
const count = (value: unknown) => (typeof value === 'number' && value >= 0 ? value : 0);
const positionOrNull = (value: unknown) =>
  Number.isSafeInteger(value) && (value as number) >= 0 ? (value as number) : null;

/** Reads `variables` of a summary. Unreadable parts are skipped as if absent (additions are compatible). */
export function readVariables(value: unknown, session: number | null): VariablesState {
  if (!isObject(value)) return { ...NO_VARIABLES, session };
  const peers: VariablePeer[] = [];
  const availability = new Map<string, Map<string, VariableAvailability>>();
  for (const p of Array.isArray(value['peers']) ? value['peers'] : []) {
    if (!isObject(p) || typeof p['name'] !== 'string') continue;
    const variables: VariableInfo[] = [];
    const states = new Map<string, VariableAvailability>();
    for (const v of Array.isArray(p['variables']) ? p['variables'] : []) {
      if (!isObject(v) || typeof v['name'] !== 'string') continue;
      const items = Array.isArray(v['enum'])
        ? v['enum'].flatMap((i) =>
            isObject(i) && typeof i['name'] === 'string' && typeof i['value'] === 'number'
              ? [{ name: i['name'], value: i['value'] }]
              : [],
          )
        : null;
      variables.push({
        name: v['name'],
        type: text(v['type']),
        role: text(v['role'], 'output'),
        writable: v['writable'] === true,
        unit: text(v['unit']),
        min: numberOrNull(v['min']),
        max: numberOrNull(v['max']),
        step: numberOrNull(v['step']),
        description: text(v['description']),
        enum: items,
      });
      states.set(v['name'], readAvailability(v['availability']));
    }
    availability.set(p['name'], states);
    peers.push({ name: p['name'], connected: p['connected'] === true, variables });
  }
  return {
    session,
    status: text(value['state'], 'unknown'),
    reason: typeof value['reason'] === 'string' ? value['reason'] : null,
    peers,
    availability,
    peersOmitted: count(value['peers_omitted']),
    variablesOmitted: count(value['variables_omitted']),
  };
}

/** Reads an `operation` notification (or one entry of operations in a summary). null if unreadable. */
export function readOperation(value: unknown, session: number): OperationView | null {
  if (
    !isObject(value) ||
    !Number.isSafeInteger(value['seq']) ||
    typeof value['state'] !== 'string'
  ) {
    return null;
  }
  const peers: Record<string, OperationPeer> = {};
  for (const [name, p] of Object.entries(isObject(value['peers']) ? value['peers'] : {})) {
    if (!isObject(p)) continue;
    peers[name] = {
      state: text(p['state'], 'unknown'),
      appliedAtMs: numberOrNull(p['applied_at_ms']),
      late: p['late'] === true,
      reason: typeof p['reason'] === 'string' ? p['reason'] : null,
      appliedWrites: positionOrNull(p['applied_writes']),
      failedWrite: positionOrNull(p['failed_write']),
    };
  }
  return {
    session,
    seq: value['seq'] as number,
    state: value['state'],
    done: value['done'] === true,
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

/** A write waiting for its answer. */
interface Pending {
  /**
   * The run the write was aimed at (the run when it was written), and the operation number (null until the HTTP
   * answer).
   */
  session: number;
  seq: number | null;
  resolve: (operation: OperationView) => void;
  reject: (error: WriteError) => void;
  timer: ReturnType<typeof setTimeout> | undefined;
  /**
   * The operations part of the last summary that arrived while waiting for the HTTP answer (number) (checked once the
   * number is known).
   */
  summary?: Listed;
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

/**
 * If the summary shows that the result of the operation with that number will no longer arrive, returns that error
 * (null if it cannot tell). The summary's list holds every unsettled operation and every operation numbered at or
 * above the boundary (operations_from_seq) that the server receives (a scenario's own writes never come; a write of
 * this client always does). So an operation that is not in the list and is older than the boundary
 * settled while disconnected, and its notification will not come any more. Operations in the list, and those numbered
 * at or above the boundary, just have not arrived yet, so keep waiting. A summary that does not give the boundary
 * cannot tell them apart, so keep waiting.
 *
 * When the server could not keep all unsettled operations (operations_open_omitted is 1 or more), an old operation not
 * in the list cannot be told apart as settled or unsettled. The server no longer follows it, so it also becomes "no
 * answer came" (told apart by the text; no-answer means "unknown whether it was applied").
 */
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

/** What VariablesFeed asks of the connection. */
export interface VariablesLink {
  /** Sends if connected (does nothing otherwise; resume() sends what is needed on connect). */
  send(message: unknown): unknown;
  onBatch(listener: (batch: readonly unknown[], current?: () => boolean) => void): () => void;
}

/**
 * ECU variables: list and state, value watches, writes and their results. No framework needed.
 *
 * - List and state come from `variables` in hello and in HTTP summaries, and from `variables` notifications (`session`
 *   notifications do not carry them). When the run changes, they are unknown until that run's `variables`
 *   notification arrives.
 * - Each variable goes into the subscription with the shortest interval requested for it (one subscription per
 *   interval). A changed subscription is resent with the same id (the server replaces it); after a reconnect, resume()
 *   resends them all.
 * - Per variable, keeps the last value and as many history points as requested (ring buffer). Dropped when the run
 *   changes.
 * - Write results arrive in `operation` notifications. They can arrive before the HTTP answer (the operation number),
 *   so recent operations are kept and matched once the number is known.
 */
export class VariablesFeed implements Omit<ClientVariables, 'bind'> {
  private instanceId: string | null = null;
  private readonly stateValue: Writable<VariablesState>;
  private readonly operationsValue: Writable<readonly OperationView[]>;
  private readonly handles = new Set<Handle>();
  private readonly buckets = new Map<number, Bucket>();
  private readonly readings = new Map<string, VariableReading>();
  private readonly histories = new Map<string, History>();
  private readonly pending = new Set<Pending>();
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
  ) {
    this.stateValue = new Writable<VariablesState>(NO_VARIABLES, Object.is, onError);
    this.operationsValue = new Writable<readonly OperationView[]>([], Object.is, onError);
    this.problemValue = new Writable<WatchProblem | null>(null, Object.is, onError);
    this.state = this.stateValue.readonly();
    this.operations = this.operationsValue.readonly();
    this.watchProblem = this.problemValue.readonly();
    link.onBatch((batch, current) => this.receive(batch, current));
  }

  // --- Connection -----------------------------------------------------------------------------

  /** Messages sent right after connecting (after authentication): all current subscriptions. */
  resume(): unknown[] {
    this.syncQueued = false;
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
    for (const pending of [...this.pending]) {
      this.settle(
        pending,
        new WriteError('closed', 'the client was closed before the result came'),
      );
    }
  }

  // --- Notifications --------------------------------------------------------------------------

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
        case 'hello': {
          flush();
          if (!current()) return;
          const summary = message['session'];
          this.adoptInstance(message['instance'], current);
          if (!current()) return;
          if (isObject(summary) && Number.isSafeInteger(summary['id'])) {
            this.adoptSummary(summary, current);
          } else {
            this.enter(null);
            if (!current()) return;
            this.stateValue.set({ ...NO_VARIABLES, status: 'off' });
          }
          touchedAll = true;
          break;
        }
        case 'session': {
          const summary = message['session'];
          const id = isObject(summary) ? summary['id'] : null;
          // Another run started: its variable notification (variables) follows. Unknown until then
          if (Number.isSafeInteger(id) && id !== this.stateValue.get().session) {
            flush();
            if (!current()) return;
            this.enter(id as number);
            if (!current()) return;
            this.stateValue.set({ ...NO_VARIABLES, session: id as number });
            touchedAll = true;
          }
          break;
        }
        case 'variables': {
          const id = message['session'];
          if (!Number.isSafeInteger(id)) break;
          if (id !== this.stateValue.get().session) {
            flush();
            if (!current()) return;
            this.enter(id as number);
            if (!current()) return;
            touchedAll = true;
          }
          this.stateValue.set(readVariables(message['variables'], id as number));
          break;
        }
        case 'values': {
          if (message['session'] !== this.stateValue.get().session) break;
          this.takeValues(message, touched);
          break;
        }
        case 'operation': {
          const id = message['session'];
          if (id !== this.stateValue.get().session) break;
          const operation = readOperation(message, id as number);
          if (operation) {
            operations = this.withOperation(operations ?? this.operationsValue.get(), operation);
            fresh.push(operation);
          }
          break;
        }
        case 'availability': {
          // Replaces the states of each participant listed. A disconnection changes nothing: the last states stay
          if (message['session'] !== this.stateValue.get().session) break;
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

  /** A reused numeric session on another server is a different run. */
  adoptInstance(value: unknown, current: () => boolean = () => true): void {
    if (typeof value !== 'string' || !value) return;
    const changed = this.instanceId !== null && this.instanceId !== value;
    this.instanceId = value;
    if (changed) {
      this.enter(null);
      if (!current()) return;
      this.stateValue.set(NO_VARIABLES);
      if (!current()) return;
      this.publish(null, current);
    }
  }

  /** Applies the variables part of hello and of an HTTP summary (`session`). */
  adoptSummary(summary: unknown, current: () => boolean = () => true): void {
    if (!isObject(summary) || !Number.isSafeInteger(summary['id'])) return;
    const id = summary['id'] as number;
    const variables = summary['variables'];
    if (id !== this.stateValue.get().session) {
      this.enter(id);
      if (!current()) return;
      this.publish(null, current);
      if (!current()) return;
    } else if (variables === undefined) {
      return; // A summary of the same run without the variables field (a session notification): keep the current state
    }
    this.stateValue.set(readVariables(variables, id));
    if (!current()) return;
    if (isObject(variables) && Array.isArray(variables['operations'])) {
      let operations = this.operationsValue.get();
      const listed: OperationView[] = [];
      for (const entry of variables['operations']) {
        const operation = readOperation(entry, id);
        if (operation) {
          listed.push(operation);
          operations = this.withOperation(operations, operation);
        }
      }
      this.setOperations(operations, listed, current);
      if (!current()) return;
      const fromSeq = variables['operations_from_seq'];
      const openOmitted = variables['operations_open_omitted'];
      this.forgetLost(id, {
        operations: listed,
        fromSeq: Number.isSafeInteger(fromSeq) ? (fromSeq as number) : null,
        openOmitted: typeof openOmitted === 'number' && openOmitted > 0,
      });
    }
  }

  /**
   * With the summary after a reconnect (or from api/session), settles as "no answer came" (lost) the writes waiting for
   * a result that will no longer arrive. For writes whose number is not known yet (before the HTTP answer), the summary
   * is kept and checked the same way once the number is known.
   */
  private forgetLost(session: number, summary: Listed): void {
    for (const pending of [...this.pending]) {
      if (pending.session !== session) continue;
      if (pending.seq === null) {
        pending.summary = summary;
        continue;
      }
      const error = lost(pending.seq, summary);
      if (error) this.settle(pending, error);
    }
  }

  /**
   * Switched to another run: drop the previous run's values, histories and operations (histories not requested by name
   * are dropped with their memory, so they do not keep growing as the variables change from run to run). Writes
   * waiting for the previous run's result are failed, since the result will not come.
   */
  private enter(session: number | null): void {
    if (session === this.stateValue.get().session) return;
    // Clear every observable part before notifying any subscriber. A subscriber can synchronously
    // fetch the new run, so it must never observe or revive the previous run's values or schema.
    this.readings.clear();
    this.histories.clear();
    this.historyReserved = 0;
    const notifyOperations = this.operationsValue.setDeferred([]);
    const notifyState = this.stateValue.setDeferred({ ...NO_VARIABLES, session });
    const notifyWatches = [...this.handles].map((handle) => handle.values.setDeferred({}));
    for (const pending of [...this.pending]) {
      if (pending.session !== session) {
        this.settle(
          pending,
          new WriteError(
            'no-answer',
            `the run ${pending.session} is over (another one has started): the result of the write ` +
              (pending.seq === null ? '' : `(operation ${pending.seq}) `) +
              'did not come',
          ),
        );
      }
    }
    notifyOperations();
    notifyState();
    for (const notify of notifyWatches) notify();
  }

  private takeValues(message: Fields, touched: Set<string>): void {
    const peers = message['peers'];
    if (!isObject(peers)) return;
    const simMs = numberOrNull(message['sim_ms']);
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
        const before = this.readings.get(key);
        if (
          before &&
          before.sampleMs === sampleMs &&
          before.simMs === simMs &&
          before.paused === paused &&
          Object.is(before.value, value)
        ) {
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

  // --- Operations -----------------------------------------------------------------------------

  private withOperation(
    operations: readonly OperationView[],
    operation: OperationView,
  ): OperationView[] {
    const at = operations.findIndex((o) => o.seq === operation.seq);
    // A settled operation never changes again (an old state arriving late does not set it back)
    if (at >= 0 && operations[at]?.done && !operation.done) return [...operations];
    const next =
      at >= 0 ? operations.map((o, i) => (i === at ? operation : o)) : [...operations, operation];
    return next.length > OPERATIONS_KEPT ? next.slice(next.length - OPERATIONS_KEPT) : next;
  }

  /**
   * Replaces the list of operations and clears the writes whose result has settled. `fresh` holds the operations that
   * just arrived (a batch of notifications, the list in a summary): the list is cut to OPERATIONS_KEPT entries, so
   * settled operations cut from the list are picked up from here.
   */
  private setOperations(
    operations: readonly OperationView[],
    fresh: readonly OperationView[] = [],
    current: () => boolean = () => true,
  ): void {
    this.operationsValue.set(operations);
    if (!current()) return;
    for (const pending of [...this.pending]) {
      if (pending.seq === null) continue;
      const mine = (o: OperationView) =>
        o.done && o.seq === pending.seq && o.session === pending.session;
      const operation = fresh.find(mine) ?? operations.find(mine);
      if (operation) this.settle(pending, operation);
    }
  }

  private settle(pending: Pending, outcome: OperationView | WriteError): void {
    if (!this.pending.delete(pending)) return;
    clearTimeout(pending.timer);
    if (outcome instanceof WriteError) {
      pending.reject(outcome);
    } else if (outcome.state === 'applied') {
      pending.resolve(outcome);
    } else {
      const reasons = Object.entries(outcome.peers)
        .filter(([, p]) => p.state !== 'applied')
        .map(([name, p]) => `${name}: ${p.state}${p.reason ? ` (${p.reason})` : ''}`);
      pending.reject(
        new WriteError(
          'failed',
          `the write (operation ${outcome.seq}) was not applied: ${reasons.join('; ') || outcome.state}`,
          null,
          outcome,
        ),
      );
    }
  }

  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options: WriteOptions = {},
  ): Promise<OperationView> {
    // Timeout: no timer for infinity (the end of the run and close() always clear it). A non-number or 0 or less means
    // the default. An overly long value is capped at the longest a timer accepts (passed as is, it would time out at
    // once)
    const asked = options.timeoutMs;
    const timeoutMs =
      asked === Infinity
        ? Infinity
        : typeof asked === 'number' && asked > 0
          ? Math.min(asked, TIMER_MAX_MS)
          : WRITE_TIMEOUT_MS;
    // Aim the write at the current run. The server refuses if that run is no longer running (session-changed)
    const session = this.stateValue.get().session;
    if (session === null) {
      return Promise.reject(
        new WriteError('refused', 'there is no run to write to', 'not-running'),
      );
    }
    const body: Fields = { session, writes };
    if (options.atMs !== undefined) body['at_ms'] = options.atMs;
    if (options.late !== undefined) body['late'] = options.late;
    return new Promise<OperationView>((resolve, reject) => {
      const pending: Pending = { session, seq: null, resolve, reject, timer: undefined };
      this.pending.add(pending);
      const known = () =>
        this.operationsValue
          .get()
          .find((o) => o.seq === pending.seq && o.session === pending.session);
      this.http({ method: 'POST', path: 'api/write', body }, (outcome) => {
        if (!this.pending.has(pending)) return; // close() failed it first
        if (!outcome.ok) {
          const error = outcome.error;
          const code = error instanceof HttpError ? (error.problem?.code ?? null) : null;
          // No answer (status 0), the Control Server did not answer in time (504), the connection broke midway (502),
          // the server failed midway (500): it may have been received. It is refused only when it is known not to have
          // been sent
          const unsure = error instanceof HttpError && [0, 500, 502, 504].includes(error.status);
          this.settle(
            pending,
            new WriteError(
              unsure ? 'no-answer' : 'refused',
              unsure
                ? `${errorMessage(error)} (the write may have been taken: look at the operations ` +
                    'and the values before writing again)'
                : errorMessage(error),
              code,
            ),
          );
          return;
        }
        const answer = outcome.body;
        const seq = isObject(answer) ? answer['operation'] : null;
        const answered = isObject(answer) ? answer['session_id'] : null;
        if (!Number.isSafeInteger(seq) || !Number.isSafeInteger(answered)) {
          this.settle(
            pending,
            new WriteError(
              'no-answer',
              'the server answered the write without an operation number',
            ),
          );
          return;
        }
        // The run changed before the answer (normally enter() fails it first): that run's result will not arrive any
        // more
        if (answered !== pending.session || this.stateValue.get().session !== pending.session) {
          this.settle(
            pending,
            new WriteError(
              'no-answer',
              `the run ${pending.session} ended before the answer to the write came (operation ${String(seq)}): ` +
                'its result will not come; look at the values before writing again',
            ),
          );
          return;
        }
        pending.seq = seq as number;
        if (options.onAccepted) {
          try {
            options.onAccepted({ session: pending.session, seq: pending.seq });
          } catch (error) {
            this.onError(error);
          }
          if (!this.pending.has(pending)) return; // onAccepted called close()
        }
        if (timeoutMs !== Infinity) {
          pending.timer = setTimeout(() => {
            const last = known();
            this.settle(
              pending,
              new WriteError(
                'no-answer',
                `no result of the write (operation ${pending.seq}) came in ${timeoutMs / 1000} s` +
                  (last
                    ? `: it is ${last.state} (it is applied when the simulation reaches its step)`
                    : ''),
                null,
                last ?? null,
              ),
            );
          }, timeoutMs);
          // Node: do not keep the process alive just for this timer
          (pending.timer as { unref?: () => void }).unref?.();
        }
        // The result may have arrived before the HTTP answer (in a notification, or in a summary after a reconnect
        // while waiting for the answer, which may show that the result will no longer arrive)
        const summary = pending.summary;
        const already =
          known() ??
          summary?.operations.find((o) => o.seq === seq && o.session === pending.session);
        if (already?.done) this.settle(pending, already);
        else if (summary) {
          const error = lost(pending.seq, summary);
          if (error) this.settle(pending, error);
        }
      });
    });
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
    for (const key of this.readings.keys()) if (!seen.has(key)) this.readings.delete(key);
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
    // With many long names, one message would exceed the server's limit (the server closes the connection on such a
    // message). Ask for all instead (and use only the watched variables on receipt)
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
