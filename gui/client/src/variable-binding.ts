import type { VariableAvailability } from './protocol';
import { ConnectionState } from './connection';
import { SessionView, isHolding } from './session-state';
import { ErrorSink, Readable, Writable, callEach } from './values';
import {
  ValueCondition,
  WriteBlock,
  WriteStatus,
  WriteValue,
  hasRange,
  sameCondition,
  valueCondition,
  writeBlock,
} from './variable-state';
import {
  HistoryPoint,
  OperationView,
  VariableInfo,
  VariableReading,
  VariableRef,
  VariableWatch,
  VariablesState,
  WatchOptions,
  WriteError,
  WriteOptions,
} from './variables';

// Binding of one variable to one screen widget (client.variables.bind). Combines the value state, whether the variable
// can be written and why not, and the write status into one readable value, and keeps one write status per variable
// and per run. No framework needed.

/** Options of client.variables.bind. */
export interface BindOptions {
  /** Interval at which values are received (ms; default WATCH_PERIOD_MS). */
  periodMs?: number;
  /** Number of history points to keep (for graphs; default 0). */
  history?: number;
  /** Do not allow writes to a variable without a range (min and max) (sliders). Meanwhile block is `no-range`. */
  needsRange?: boolean;
  /**
   * Do not allow writes while there is no value (read or sent) (widgets that move from the value: sliders). Meanwhile
   * block is `no-value`.
   */
  needsValue?: boolean;
  /**
   * Whether to receive values from the start (default true). If false, nothing is watched until setVisible(true) is
   * called (so that only widgets visible on the screen receive values).
   */
  visible?: boolean;
}

/** Current state of one variable (the value of VariableBinding.state). */
export interface BoundVariable {
  /** Which variable. */
  readonly ref: VariableRef;
  /** Definition of the variable (undefined if it is not in the current run). */
  readonly info: VariableInfo | undefined;
  /** The last value (undefined if none yet, or if values are not received (visible is false)). */
  readonly reading: VariableReading | undefined;
  /**
   * Whether the ECU uses the variable now (`unknown` if the variable is not in the current run). Information only: it
   * does not enter condition or block, and a disconnection does not change it.
   */
  readonly availability: VariableAvailability;
  /** State of the value (stale, none yet, the value at the paused time and so on). */
  readonly condition: ValueCondition;
  /** Why the variable cannot be written now (null if it can). */
  readonly block: WriteBlock | null;
  /**
   * Status of the last write to this variable (in the current run). Every bind of the same variable sees the same
   * status.
   */
  readonly writeStatus: WriteStatus;
  /**
   * The value sent whose result has not arrived yet (null if none). If set, a widget shows it instead of the value
   * read (so a switch that was clicked does not appear to flip back while the result is pending). Cleared when the run
   * changes.
   */
  readonly pending: WriteValue | null;
}

/**
 * Binding to one variable. Call close() when done with it.
 *
 * ```js
 * const throttle = client.variables.bind({ peer: 'Engine', name: 'engine.throttle' }, { needsRange: true });
 * throttle.state.subscribe(({ reading, pending, block }) => {
 *   slider.disabled = block !== null;
 *   slider.value = pending ?? reading?.value ?? 0;
 * });
 * slider.onchange = () => throttle.write(Number(slider.value));
 * ```
 */
export interface VariableBinding {
  /** Which variable. */
  readonly ref: VariableRef;
  /**
   * The current state. get() always builds it from the current state (it does not depend on the order of subscribers).
   */
  readonly state: Readable<BoundVariable>;
  /**
   * History (oldest first; only when BindOptions.history asks for it and values are being received; empty otherwise). A
   * new array on every call.
   */
  history(): HistoryPoint[];
  /**
   * Writes. Does nothing when the variable cannot be written (block of state). If the server has not received the
   * previous write yet, the value is held instead of sent, and once the server receives it only the last value is sent
   * (so dragging a slider or repeating a key does not pile up writes; at that point it checks again that the run is the
   * same and the variable can still be written, and if not, nothing is sent and writeStatus is `dropped`). A write
   * whose result never came is not resent. The result appears in writeStatus of state (no Promise is returned).
   */
  write(value: WriteValue): void;
  /**
   * Shows an error the widget found before sending (not a number, out of range and so on) as the write status
   * (`invalid`). Leaves alone the write being sent, the write waiting for its result and the next value held.
   */
  reject(message: string): void;
  /**
   * Whether to receive values (widgets that watch only while visible call it whenever they show or hide). While false,
   * reading is undefined.
   */
  setVisible(visible: boolean): void;
  /** Stops (cleans up the watch and the subscriptions). write, reject and setVisible do nothing afterwards. */
  close(): void;
}

/** Text used when a held value was not sent (dropped of WriteStatus). */
export const DROPPED_MESSAGE =
  'the value kept back was not sent: the variable cannot be written any more ' +
  '(the run ended or another one started, or the ECU is not connected). ' +
  'Check the value and write it again';

/** What bind reads (the client's state). */
export interface BindingSources {
  readonly variables: {
    readonly state: Readable<VariablesState>;
    watch(variables: readonly VariableRef[], options?: WatchOptions): VariableWatch;
    write(
      writes: Record<string, Record<string, number | boolean | string>>,
      options?: WriteOptions,
    ): Promise<OperationView>;
  };
  readonly connection: Readable<ConnectionState>;
  readonly session: Readable<SessionView | null>;
  readonly onError: ErrorSink;
}

/** One write sent (writes are compared by object identity). */
interface Sent {
  readonly value: WriteValue;
}

/**
 * Write status of one variable in one run. A write goes from sent (sending) to received (waiting for the result) to
 * settled. A write refused before it is received is settled at that point.
 *
 * - `sending`: the write the server has not received yet (at most one). While it exists, the next value is held in
 *   `queued` instead of sent. Cleared only when that write is accepted or settles.
 * - `last`: the last write sent, while it has not settled. Owner of `pending` (the "value sent" a widget shows).
 *   Cleared only when that write settles (sending a new one replaces it).
 * - `shown`: the write that `status` (the status field) follows. Replaced when a new value is sent or held; an error
 *   before sending (reject) and "not sent" (dropped) set it to null (no write followed) and show their own text. The
 *   acceptance and settling of a write that is not followed do not change the status field.
 *
 * An error before sending changes only `shown` and `status`; it leaves `sending`, `queued`, `last` and `pending`
 * alone.
 */
interface Entry {
  session: number;
  key: string;
  ref: VariableRef;
  status: WriteStatus;
  pending: WriteValue | null;
  sending: Sent | null;
  queued: { value: WriteValue } | null;
  last: Sent | null;
  shown: Sent | null;
}

const keyOf = (ref: VariableRef) => `${ref.peer}\u0000${ref.name}`;

const IDLE: WriteStatus = { kind: 'idle' };

/**
 * The inside of bind: lookup of variable definitions, and write statuses. One per client. Binding the same variable in
 * several places never makes writes overlap, and recreating a bind does not lose the in-flight or failure status.
 * Statuses are per run and start empty when the run changes (a previous run's "applied" or "value sent" is never
 * shown for a new run).
 */
export class VariableBinder {
  private entries = new Map<string, Entry>();
  private entriesSession: number | null = null;
  /** Per variable, the functions to call when the write status changes (the binds listen). */
  private readonly listeners = new Map<string, Set<() => void>>();
  /** Lookup table of the list (rebuilt when peers change). */
  private tableOf: VariablesState['peers'] | null = null;
  private table = new Map<string, Map<string, VariableInfo>>();
  private connectedPeers = new Set<string>();

  constructor(readonly sources: BindingSources) {}

  /** Binds to a variable (ClientVariables.bind). */
  bind(ref: VariableRef, options: BindOptions = {}): VariableBinding {
    return new Binding(this, { peer: ref.peer, name: ref.name }, options);
  }

  private lookUp(): void {
    const peers = this.sources.variables.state.get().peers;
    if (peers === this.tableOf) return;
    this.tableOf = peers;
    this.table = new Map();
    this.connectedPeers = new Set();
    for (const peer of peers) {
      this.table.set(peer.name, new Map(peer.variables.map((v) => [v.name, v])));
      if (peer.connected) this.connectedPeers.add(peer.name);
    }
  }

  /** Definition of the variable (undefined if it is not in the current run). */
  find(ref: VariableRef): VariableInfo | undefined {
    this.lookUp();
    return this.table.get(ref.peer)?.get(ref.name);
  }

  /** Whether that participant is connected to the Control Server now. */
  peerConnected(peer: string): boolean {
    this.lookUp();
    return this.connectedPeers.has(peer);
  }

  /** Whether the connection between the client and the GUI server is open. */
  connected(): boolean {
    return this.sources.connection.get() === 'open';
  }

  /**
   * Why the variable cannot be written now (null if it can). Only the widget-independent reasons (Binding adds
   * scenario, range and value).
   */
  block(ref: VariableRef): WriteBlock | null {
    return writeBlock({
      info: this.find(ref),
      status: this.sources.variables.state.get().status,
      connected: this.connected(),
      beforeStart: isHolding(this.sources.session.get()),
      peerConnected: this.peerConnected(ref.peer),
    });
  }

  /** Status of the last write to that variable in the current run. */
  status(ref: VariableRef): WriteStatus {
    return this.entry(ref)?.status ?? IDLE;
  }

  /** The value sent whose result has not arrived yet (null if none). */
  pending(ref: VariableRef): WriteValue | null {
    return this.entry(ref)?.pending ?? null;
  }

  /** Called when the write status of that variable changes. Call the returned function to stop. */
  onChange(ref: VariableRef, listener: () => void): () => void {
    const key = keyOf(ref);
    const set = this.listeners.get(key) ?? new Set<() => void>();
    this.listeners.set(key, set);
    set.add(listener);
    return () => {
      set.delete(listener);
      if (set.size === 0 && this.listeners.get(key) === set) this.listeners.delete(key);
    };
  }

  private changed(entry: Entry): void {
    const set = this.listeners.get(entry.key);
    if (set) callEach(set, [], this.sources.onError);
  }

  /**
   * Writes. Does nothing when the variable cannot be written. If the server has not received the previous write yet,
   * the value is held instead of sent, and once the server receives it only the last value is sent (at that point it
   * checks again that the run is the same and the variable can still be written). A write whose result never came is
   * not resent.
   */
  write(ref: VariableRef, value: WriteValue): void {
    if (this.block(ref) !== null) return;
    const entry = this.entry(ref);
    if (!entry) return;
    if (entry.sending) {
      // The status field follows the write being sent (if it is refused, the held value is not sent either, so show
      // why)
      entry.queued = { value };
      entry.shown = entry.sending;
      entry.status = { kind: 'sending' };
      entry.pending = value;
      this.changed(entry);
      return;
    }
    this.send(entry, value);
  }

  /**
   * Shows an error the widget found before sending (not a number, out of range and so on) as the write status. Leaves
   * alone the write being sent, the write waiting for its result and the next value held (the result of a write
   * already sent no longer appears in the status field, but the value-sent display is cleared when it settles; if the
   * held value is sent later, the status field follows that write).
   */
  reject(ref: VariableRef, message: string): void {
    const entry = this.entry(ref);
    if (!entry) return;
    entry.shown = null;
    entry.status = { kind: 'invalid', message };
    this.changed(entry);
  }

  /**
   * Status of that variable in the current run (undefined without a run). When the run changes, the previous run's
   * statuses are dropped.
   */
  private entry(ref: VariableRef): Entry | undefined {
    const session = this.sources.variables.state.get().session;
    if (session !== this.entriesSession) {
      this.entries = new Map();
      this.entriesSession = session;
    }
    if (session === null) return undefined;
    const key = keyOf(ref);
    let entry = this.entries.get(key);
    if (!entry) {
      entry = {
        session,
        key,
        ref: { peer: ref.peer, name: ref.name },
        status: IDLE,
        pending: null,
        sending: null,
        queued: null,
        last: null,
        shown: null,
      };
      this.entries.set(key, entry);
    }
    return entry;
  }

  private send(entry: Entry, value: WriteValue): void {
    const mine: Sent = { value };
    const { peer, name } = entry.ref;
    entry.sending = mine;
    entry.last = mine;
    entry.shown = mine;
    entry.status = { kind: 'sending' };
    entry.pending = value;
    this.changed(entry);
    /** The server received it: send the held next value, if any. */
    const accepted = (seq: number | null) => {
      if (entry.sending !== mine) return;
      entry.sending = null;
      const next = entry.queued;
      entry.queued = null;
      if (!next) {
        if (entry.shown === mine && seq !== null) {
          entry.status = { kind: 'queued', seq };
          this.changed(entry);
        }
        return;
      }
      // The held value: sent only in the same run while the variable can still be written. variables.write promises not
      // to call onAccepted after the run changes, so same is always true here (kept as a guard in case that promise
      // changes)
      const same = this.sources.variables.state.get().session === entry.session;
      if (same && this.block(entry.ref) === null) {
        this.send(entry, next.value);
        return;
      }
      // Not sent. What was shown was the unsent value, so clear it (the result of mine, which was received, does not
      // appear in the status field)
      entry.pending = null;
      entry.shown = null;
      entry.status = { kind: 'dropped', message: DROPPED_MESSAGE };
      this.changed(entry);
    };
    /** Settled (applied, failed, refused, no answer): no further result will come. */
    const settled = (status: WriteStatus) => {
      if (entry.sending === mine) {
        // Ended before it was received: do not send the held next value either (it would be refused for the same
        // reason)
        entry.sending = null;
        entry.queued = null;
      }
      if (entry.last === mine) {
        entry.last = null;
        entry.pending = null;
      }
      if (entry.shown === mine) entry.status = status;
      this.changed(entry);
    };
    this.sources.variables
      .write(
        { [peer]: { [name]: value } },
        {
          // While paused, the result does not come until time advances. Do not time out; wait for the result of the
          // operation (if the result is lost because the run ends, the run changes or the connection is re-established,
          // write settles it as "no answer came")
          timeoutMs: Infinity,
          onAccepted: ({ seq }) => accepted(seq),
        },
      )
      .then(
        (operation) => {
          accepted(null); // Usually onAccepted has already done this
          settled({ kind: 'applied', atMs: operation.peers[peer]?.appliedAtMs ?? null });
        },
        (error: unknown) =>
          settled(
            error instanceof WriteError
              ? { kind: error.kind, message: error.message, code: error.code }
              : { kind: 'no-answer', message: String(error), code: null },
          ),
      );
  }
}

function sameBound(a: BoundVariable, b: BoundVariable): boolean {
  return (
    a.info === b.info &&
    a.availability === b.availability &&
    a.reading === b.reading &&
    a.block === b.block &&
    a.writeStatus === b.writeStatus &&
    a.pending === b.pending &&
    sameCondition(a.condition, b.condition)
  );
}

/** One bind. Rebuilds state whenever the client's state or the write status changes. */
class Binding implements VariableBinding {
  readonly state: Readable<BoundVariable>;
  private readonly value: Writable<BoundVariable>;
  private readonly stops: (() => void)[] = [];
  private watch: VariableWatch | null = null;
  private stopWatch: (() => void) | null = null;
  private open = true;

  constructor(
    private readonly binder: VariableBinder,
    readonly ref: VariableRef,
    private readonly options: BindOptions,
  ) {
    const { sources } = binder;
    if (options.visible ?? true) this.openWatch();
    this.value = new Writable(this.compute(), sameBound, sources.onError);
    const refresh = () => this.refresh();
    this.stops.push(
      sources.variables.state.subscribe(refresh),
      sources.connection.subscribe(refresh),
      sources.session.subscribe(refresh),
      binder.onChange(ref, refresh),
    );
    this.state = {
      // Built from the current state on every read (never returns a stale status, even if another subscriber was called
      // first)
      get: () => {
        this.refresh();
        return this.value.get();
      },
      subscribe: (listener) => this.value.subscribe(listener),
    };
  }

  private refresh(): void {
    if (this.open) this.value.set(this.compute());
  }

  private compute(): BoundVariable {
    const { binder, ref } = this;
    const { sources } = binder;
    const variables = sources.variables.state.get();
    const info = binder.find(ref);
    const reading = this.watch?.value(ref);
    const condition = valueCondition({
      status: variables.status,
      known: info !== undefined,
      reading,
      connected: binder.connected(),
      peerConnected: binder.peerConnected(ref.peer),
      // A reason while still ready means reading values keeps failing (the value is the last one read)
      readFailing: variables.status === 'ready' && variables.reason !== null,
    });
    const pending = binder.pending(ref);
    return {
      ref,
      info,
      availability: info?.availability ?? 'unknown',
      reading,
      condition,
      block: this.blockOf(info, reading, pending),
      writeStatus: binder.status(ref),
      pending,
    };
  }

  private blockOf(
    info: VariableInfo | undefined,
    reading: VariableReading | undefined,
    pending: WriteValue | null,
  ): WriteBlock | null {
    const base = this.binder.block(this.ref);
    if (base !== null) return base;
    // While a scenario runs, the variable is watch-only (no write status is kept; the server refuses writes too)
    const session = this.binder.sources.session.get();
    if (session?.scenario && session.launcher.running) return 'scenario';
    if (this.options.needsRange && info && !hasRange(info)) return 'no-range';
    if (this.options.needsValue && reading === undefined && pending === null) return 'no-value';
    return null;
  }

  private openWatch(): void {
    const { periodMs, history } = this.options;
    const watch = this.binder.sources.variables.watch([this.ref], { periodMs, history });
    this.watch = watch;
    this.stopWatch = watch.values.subscribe(() => this.refresh());
  }

  private closeWatch(): void {
    this.stopWatch?.();
    this.stopWatch = null;
    this.watch?.close();
    this.watch = null;
  }

  history(): HistoryPoint[] {
    return this.options.history && this.watch ? this.watch.history(this.ref) : [];
  }

  write(value: WriteValue): void {
    if (!this.open) return;
    if (this.compute().block !== null) return;
    this.binder.write(this.ref, value);
  }

  reject(message: string): void {
    if (this.open) this.binder.reject(this.ref, message);
  }

  setVisible(visible: boolean): void {
    if (!this.open || visible === (this.watch !== null)) return;
    if (visible) this.openWatch();
    else this.closeWatch();
    this.refresh();
  }

  close(): void {
    if (!this.open) return;
    this.open = false;
    for (const stop of this.stops) stop();
    this.closeWatch();
  }
}
