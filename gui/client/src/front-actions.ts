// The Promise operations of the front door. A time command carries an `id` that this page chose, and its answer is the
// launcher's `command` event with the same `id`; the answer timeout belongs to this file (not to `client.time`, which
// stops waiting when any command event arrives). `step` and `runUntil` resolve when time has arrived and the run is
// paused again.
import type { GuiClient } from './client';
import { HttpError, HttpTransport, errorMessage } from './http';
import type { FrontControls, Intent, OperationName, RunState } from './front-types';
import { ActionError } from './front-types';
import type { Snapshot } from './front-compose';
import type { StopCapture } from './run';
import { sameRunTarget, type RunTarget } from './session';
import { isRunActive } from './session-state';
import { linkOf, linkProblem, runOf, standsAt } from './front-compose';
import type {
  CheckResult,
  CheckTextRequest,
  ConfigText,
  LauncherEvent,
  RunAck,
  SavedConfig,
  RunRequest,
  ServerMessage,
} from './protocol';
import { ErrorSink, Readable, Writable } from './values';

/** The time operations. */
type TimeOperation = 'pause' | 'resume' | 'step' | 'speed' | 'run_until';

/** A failed action, kept for `status.problem`. */
export interface Failure {
  error: ActionError;
  /** `status.run` when it happened: the failure goes when that changes. */
  run: RunState;
  /** Later is newer. */
  stamp: number;
}

/** What the operations read from the rest of the front. */
export interface ActionSources {
  low: GuiClient;
  http: HttpTransport;
  captureStop(): StopCapture;
  activeTarget?(): RunTarget | null;
  snapshot(): Snapshot;
  controls(): FrontControls;
  run(): RunState;
  /** The start-up file (null if there is none). */
  initialFile(): string | null;
  /** How long to wait for the launcher's answer to a time command (ms). */
  answerTimeoutMs: number;
  onError: ErrorSink;
  /** The next stamp (shared with the other sources of problems). */
  stamp(): number;
}

/** Turns what went wrong on the way to the server into an ActionError. */
export function actionError(error: unknown): ActionError {
  if (error instanceof ActionError) return error;
  if (error instanceof HttpError) {
    if (error.status === 0) {
      return new ActionError(
        error.timedOut ? 'no-answer' : 'network',
        error.timedOut
          ? `${error.message}: the server may have done it; look at status and clock before you send it again`
          : `${error.message}: the request did not reach the server`,
        'unknown',
      );
    }
    return new ActionError(
      error.problem?.code ?? `http-${error.status}`,
      error.message,
      'refused',
      error.problem?.findings ?? [],
    );
  }
  return new ActionError('failed', errorMessage(error), 'unknown');
}

interface Waiting {
  id: number;
  op: TimeOperation;
  /** The step's or run_until's own request: the amount for a step, the time for a run_until. */
  amount: number;
  run_id: string;
  instance: string;
  stage: 'sent' | 'moving';
  /** moving: the virtual time that has to be reached, and the time it started from. */
  target: number;
  from: number;
  /** The launcher said when it pauses (`at_ms` of the command event): the target is exact. */
  exact: boolean;
  timer: ReturnType<typeof setTimeout> | undefined;
  resolve: () => void;
  reject: (error: ActionError) => void;
}

/** Why a check that waited was not sent. */
const closedCheck = () =>
  new ActionError('closed', 'the client was closed before the check was sent', 'refused');

/** A check that waits for its turn or is on its way. */
interface QueuedCheck {
  /**
   * The request as JSON: a check of the same request shares this one. `describe:` and the request as JSON: a describe
   * of the same file and variant. null: a check of a text, which nothing shares.
   */
  key: string | null;
  /** The signal of each caller (null: one that cannot abort). */
  callers: (AbortSignal | null)[];
  promise: Promise<CheckResult>;
  /** Settles after `promise`, once the check has left the queue. */
  done: Promise<void>;
}

/**
 * The commands that make a step on its way end somewhere else: a pause or a resume from another sender. (A step or a
 * run_until sent meanwhile does not: the launcher carries the first one on and counts the next from where it stops.)
 */
const MOVES = new Set(['pause', 'resume']);

/** The fields of an `Intent` (what `run.start()` takes). */
const INTENT_FIELDS = new Set([
  'vehicle_sil_run_request',
  'disable',
  'enable',
  'manual',
  'scenario',
  'case',
  'after_scenario',
  'record',
  'record_after_fail_ms',
  'record_events',
  'record_can',
  'variant',
  'params',
  'speed',
  'start_paused',
  'stop_at_ms',
  'late_tolerance_ms',
  'strict',
  'connect',
  'time_policy',
]);

/** The fields of an `Intent` that a check of a text takes. */
const TEXT_INTENT_FIELDS = new Set(['vehicle_sil_run_request', 'variant', 'strict']);

/** Maximum delay accepted by 32-bit platform timers (`setTimeout`). */
const TIMER_MAX_MS = 2_147_483_647;

/** The refusal when there is no file to use. */
export const noFileError = (): ActionError =>
  new ActionError(
    'no-file',
    'no configuration file was given and the server has none to start with: pass { file } (a path from client.low.configs())',
    'refused',
  );

export class FrontActions {
  private readonly busyValue: Writable<ReadonlySet<OperationName>>;
  private readonly failureValue: Writable<Failure | null>;
  private readonly startedRunIdValue: Writable<string | null>;
  private readonly stopConfirmedForValue: Writable<string | null>;
  private readonly waiting = new Map<number, Waiting>();
  private readonly inFlight = new Map<
    'stop' | 'force_stop',
    { key: object | string; promise: Promise<void>; cancel: () => void }
  >();
  private readonly syncCancelers = new Set<() => void>();
  private syncingStop: Promise<void> | null = null;
  private syncingForceStop: Promise<void> | null = null;
  /** The checks not answered yet, in the order they are sent: the first is on its way, the others wait. */
  private readonly checks: QueuedCheck[] = [];
  /** Rejects when the client closes: a check that waits ends with it. */
  private close: (error: ActionError) => void = () => undefined;
  private readonly closing = new Promise<never>((_, reject) => (this.close = reject));
  private nextId: number;
  private lastSim: number | null = null;
  private readonly stops: (() => void)[] = [];
  private disposed = false;

  readonly busy: Readable<ReadonlySet<OperationName>>;
  readonly failure: Readable<Failure | null>;
  readonly startedRunId: Readable<string | null>;
  readonly stopConfirmedFor: Readable<string | null>;

  constructor(
    private readonly src: ActionSources,
    /** Where the ids start (they must differ between pages that send to the same run). */
    idStart: number = 1 + Math.floor(Math.random() * 2 ** 40),
  ) {
    this.nextId = idStart;
    this.closing.catch(() => undefined); // no check may wait when the client closes
    this.busyValue = new Writable<ReadonlySet<OperationName>>(new Set(), Object.is, src.onError);
    this.failureValue = new Writable<Failure | null>(null, Object.is, src.onError);
    this.startedRunIdValue = new Writable<string | null>(null, Object.is, src.onError);
    this.stopConfirmedForValue = new Writable<string | null>(null, Object.is, src.onError);
    this.busy = this.busyValue.readonly();
    this.failure = this.failureValue.readonly();
    this.startedRunId = this.startedRunIdValue.readonly();
    this.stopConfirmedFor = this.stopConfirmedForValue.readonly();
    this.stops.push(
      src.low.session.onMessages((batch) => this.onMessages(batch)),
      src.low.session.session.subscribe(() => {
        this.startedRunIdValue.set(null);
        this.evaluate();
      }),
    );
  }

  // --- run ---------------------------------------------------------------------------------------

  start(
    intent: Intent = {},
    options: { file?: string; onStarted?: (started: RunAck) => void } = {},
  ): Promise<void> {
    const request = this.requestOf(intent, options);
    if (request instanceof ActionError) return this.fail(request);
    const own = this.refusalOfStart();
    if (own) return this.fail(own);
    this.stopConfirmedForValue.set(null);
    // Reserve the low-level start before notifying Front subscribers: a subscriber may stop synchronously.
    const start = this.src.low.run.start(request);
    return this.guarded('start', () =>
      start.then((started) => {
        if (started) {
          if (this.src.low.session.session.get()?.id !== started.run_id) {
            this.startedRunIdValue.set(started.run_id);
          }
          options.onStarted?.(started);
        }
      }),
    );
  }

  isAwaitingRunSession(): boolean {
    const id = this.startedRunIdValue.get();
    return id !== null && this.src.low.session.session.get()?.id !== id;
  }

  isStopConfirmed(): boolean {
    const id = this.stopConfirmedForValue.get();
    const session = this.src.low.session.session.get();
    return id !== null && session !== null && isRunActive(session) && session.id === id;
  }

  /**
   * Checks what `intent` would start (`api/check`). Its failure is the caller's: it is not kept for `status.problem`.
   * One check at a time: a check is sent after the answer to the one before (the server runs them one at a time, and
   * refuses with `check-busy` when too many wait). A check of the same request as one that waits or is on its way
   * shares it (the parts and the screen of a page check the same page intent); with `fresh`, only one that waits (a
   * check that the user asks for again reads the file as it is now). A check whose callers have all aborted
   * (`signal`) while it waits is not sent, and neither is one that waits when the client closes (`closed`).
   */
  check(
    intent: Intent = {},
    options: { file?: string; text?: string; signal?: AbortSignal; fresh?: boolean } = {},
  ): Promise<CheckResult> {
    const request =
      options.text === undefined
        ? this.requestOf(intent, options)
        : this.textRequestOf(intent, options.file, options.text);
    if (request instanceof ActionError) return Promise.reject(request);
    if (this.disposed) return Promise.reject(closedCheck());
    const signal = options.signal ?? null;
    if (signal?.aborted) return Promise.reject(signal.reason as Error);
    // A check of a text takes its place in the line but shares nothing: its key is null, which no other check has
    const key = options.text === undefined ? JSON.stringify(request) : null;
    const going =
      key === null
        ? undefined
        : this.checks.find((c) => c.key === key && !(options.fresh && c === this.checks[0]));
    if (going) {
      going.callers.push(signal);
      return going.promise;
    }
    return this.enqueue(key, [signal], () => this.src.low.check(request));
  }

  /** Reads a configuration file as it is saved. Its failure is the caller's: it is not kept for `status.problem`. */
  readConfig(options: { file?: string } = {}): Promise<ConfigText> {
    const named = options.file ?? this.src.initialFile();
    if (named === null) return Promise.reject(noFileError());
    return this.src.low.config(named).catch((error: unknown) => {
      throw actionError(error);
    });
  }

  /** Saves a text as a configuration file. Its failure is the caller's: it is not kept for `status.problem`. */
  saveConfig(options: { file?: string; text: string; stamp: string | null }): Promise<SavedConfig> {
    const file = options.file ?? this.src.initialFile();
    if (file === null) return Promise.reject(noFileError());
    return this.src.low
      .saveConfig({ file, text: options.text, stamp: options.stamp })
      .catch((error: unknown) => {
        throw actionError(error);
      });
  }

  /** Describes the participants of the saved file again. It waits in the line of the checks; it shares only with a describe of the same file and variant. */
  describeAgain(options: { file?: string; variant?: string } = {}): Promise<CheckResult> {
    const file = options.file ?? this.src.initialFile();
    if (file === null) return Promise.reject(noFileError());
    if (this.disposed) return Promise.reject(closedCheck());
    const request = {
      file,
      ...(options.variant !== undefined ? { variant: options.variant } : {}),
    };
    // A describe of the same file and variant that waits or is on its way is shared (pressing again does not start
    // every executable once more). Its answer is the caller's alone: it does not go through `record()`.
    const key = `describe:${JSON.stringify(request)}`;
    const going = this.checks.find((c) => c.key === key);
    if (going) return going.promise;
    return this.enqueue(key, [null], () => this.src.low.describe(request));
  }

  /** The line of the checks: one is sent after the answer to the one before. `key` null: nothing is shared. */
  private enqueue(
    key: string | null,
    callers: (AbortSignal | null)[],
    run: () => Promise<CheckResult>,
  ): Promise<CheckResult> {
    const send = () =>
      run().catch((error: unknown) => {
        throw actionError(error);
      });
    const before = this.checks.at(-1)?.done;
    const promise = before
      ? Promise.race([
          before.then(() => {
            if (this.disposed) return Promise.reject(closedCheck());
            const aborted = callers.every((s) => s?.aborted) ? callers[0] : null;
            return aborted ? Promise.reject(aborted.reason as Error) : send();
          }),
          this.closing,
        ])
      : send();
    const leave = () => {
      this.checks.splice(
        this.checks.findIndex((c) => c.promise === promise),
        1,
      );
    };
    this.checks.push({ key, callers, promise, done: promise.then(leave, leave) });
    return promise;
  }

  /** The body of api/run and api/check for `intent`, or why it cannot be sent. */
  private requestOf(intent: Intent, options: { file?: string }): RunRequest | ActionError {
    const file = options.file ?? this.src.initialFile();
    if (file === null) return noFileError();
    // A field the client does not know (a misspelt `scenaro` from plain JavaScript) would be dropped and the run
    // started without it: refuse it instead.
    const unknown = Object.keys(intent).filter((k) => !INTENT_FIELDS.has(k));
    if (unknown.length > 0) {
      return new ActionError(
        'unknown-field',
        `the intent has ${unknown.join(', ')}, which start and check do not know: the fields are ${[...INTENT_FIELDS].join(', ')}`,
        'refused',
      );
    }
    // The server takes the intent as the request file has it: a choice that is not made is not sent (no null)
    return {
      file,
      disable: intent.disable ?? [],
      ...(intent.vehicle_sil_run_request
        ? { vehicle_sil_run_request: intent.vehicle_sil_run_request }
        : {}),
      ...(intent.enable ? { enable: intent.enable } : {}),
      ...(intent.manual ? { manual: intent.manual } : {}),
      ...(intent.scenario !== undefined ? { scenario: intent.scenario } : {}),
      ...(intent.case !== undefined ? { case: intent.case } : {}),
      ...(intent.after_scenario !== undefined ? { after_scenario: intent.after_scenario } : {}),
      ...(intent.record ? { record: intent.record } : {}),
      ...(intent.record_after_fail_ms !== undefined
        ? { record_after_fail_ms: intent.record_after_fail_ms }
        : {}),
      ...(intent.record_events ? { record_events: true } : {}),
      ...(intent.record_can ? { record_can: true } : {}),
      ...(intent.variant !== undefined ? { variant: intent.variant } : {}),
      ...(intent.params ? { params: intent.params } : {}),
      ...(intent.speed !== undefined ? { speed: intent.speed } : {}),
      ...(intent.start_paused ? { start_paused: true } : {}),
      ...(intent.stop_at_ms !== undefined ? { stop_at_ms: intent.stop_at_ms } : {}),
      ...(intent.late_tolerance_ms !== undefined
        ? { late_tolerance_ms: intent.late_tolerance_ms }
        : {}),
      ...(intent.strict ? { strict: true } : {}),
      ...(intent.connect ? { connect: intent.connect } : {}),
      ...(intent.time_policy !== undefined ? { time_policy: intent.time_policy } : {}),
    };
  }

  /** The body of api/check for a text: the server takes only the configuration (`variant`, `strict`), not a run's choices. */
  private textRequestOf(
    intent: Intent,
    fileOption: string | undefined,
    text: string,
  ): CheckTextRequest | ActionError {
    const file = fileOption ?? this.src.initialFile();
    if (file === null) return noFileError();
    const unknown = Object.keys(intent).filter((k) => !TEXT_INTENT_FIELDS.has(k));
    if (unknown.length > 0) {
      return new ActionError(
        'unknown-field',
        `the intent of a check of a text has ${unknown.join(', ')}, which it does not take: a text is checked as the configuration, so the fields are ${[...TEXT_INTENT_FIELDS].join(', ')}`,
        'refused',
      );
    }
    return {
      file,
      text,
      ...(intent.vehicle_sil_run_request
        ? { vehicle_sil_run_request: intent.vehicle_sil_run_request }
        : {}),
      ...(intent.variant !== undefined ? { variant: intent.variant } : {}),
      ...(intent.strict ? { strict: true } : {}),
    };
  }

  /** What the client refuses before it sends a start: the connection, and a start of this page still on its way. */
  private refusalOfStart(): ActionError | null {
    const link = linkOf(this.src.snapshot());
    if (link === 'rejected' || link === 'incompatible') {
      const why = this.src.controls().start.reason;
      return new ActionError(link, why?.message ?? link, 'refused');
    }
    return this.busyValue.get().has('start')
      ? new ActionError(
          'busy',
          'the previous start has not been answered yet: wait for it',
          'refused',
        )
      : null;
  }

  private timeoutDelayMs(): number {
    return Math.min(this.src.answerTimeoutMs, TIMER_MAX_MS);
  }

  /**
   * Waits while `link === 'connecting'` (bounded by `answerTimeoutMs`).
   * Resolves once `link !== 'connecting'` (or `extraDone()` holds), and never hangs on WS close,
   * auth rejection, protocol mismatch, timeout, or `dispose()`.
   */
  waitForSync(opLabel: string, recordOnTimeout = true, extraDone?: () => boolean): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', `the client was closed before ${opLabel} was sent`, 'refused'),
      );
    }
    const isDone = () => linkOf(this.src.snapshot()) !== 'connecting' || (extraDone?.() ?? false);
    if (isDone()) return Promise.resolve();

    return new Promise<void>((resolve, reject) => {
      let settled = false;
      let timer: ReturnType<typeof setTimeout> | undefined = undefined;
      const unsubscribes: (() => void)[] = [];
      const cleanup = () => {
        if (settled) return false;
        settled = true;
        clearTimeout(timer);
        this.syncCancelers.delete(onDispose);
        for (const off of unsubscribes) off();
        return true;
      };
      const onDispose = () => {
        if (cleanup()) {
          reject(
            new ActionError(
              'closed',
              `the client was closed before ${opLabel} was sent`,
              'refused',
            ),
          );
        }
      };
      const check = () => {
        if (this.disposed) {
          onDispose();
          return;
        }
        if (isDone() && cleanup()) {
          resolve();
        }
      };

      this.syncCancelers.add(onDispose);
      const watchSources: Readable<unknown>[] = [
        this.src.low.connection.state,
        this.src.low.connection.failures,
        this.src.low.connection.lastClose,
        this.src.low.session.rejected,
        this.src.low.session.incompatible,
        this.src.low.session.ready,
        this.src.low.run.starting,
        this.startedRunId,
      ];
      for (const source of watchSources) {
        unsubscribes.push(source.subscribe(check));
      }
      if (settled) {
        for (const off of unsubscribes) off();
        return;
      }
      timer = setTimeout(() => {
        if (cleanup()) {
          const error = new ActionError(
            'timeout',
            `initial WebSocket sync did not complete within ${this.src.answerTimeoutMs / 1000} s before ${opLabel}: try again once status.link is online`,
            'refused',
          );
          reject(recordOnTimeout ? this.record(error) : error);
        }
      }, this.timeoutDelayMs());
      check();
    });
  }

  private hasLocalStart(): boolean {
    if (this.src.low.run.starting.get() || this.startedRunIdValue.get() !== null) return true;
    const active = this.src.activeTarget?.() ?? null;
    return active !== null && !sameRunTarget(active, this.src.low.session.currentTarget());
  }

  private needsSyncForStop(): boolean {
    return !this.hasLocalStart() && linkOf(this.src.snapshot()) === 'connecting';
  }

  private isStoppable(s: Snapshot): boolean {
    const run = runOf(s);
    const live = isRunActive(s.session) && run !== 'ended' && run !== 'crashed';
    return (
      this.hasLocalStart() ||
      s.busy.has('start') ||
      s.awaitingRunSession === true ||
      live ||
      run === 'starting' ||
      s.allowed?.stop.ok === true ||
      s.allowed?.force.ok === true
    );
  }

  private refusalBeforeStop(s: Snapshot, opLabel: 'stop' | 'force_stop'): ActionError | null {
    if (this.disposed) {
      return new ActionError(
        'closed',
        `the client was closed before ${opLabel} was sent`,
        'refused',
      );
    }
    const link = linkOf(s);
    if (link === 'rejected' || link === 'incompatible') {
      const why = linkProblem(s);
      return new ActionError(link, why?.message ?? link, 'refused');
    }
    const hasLocalStart = this.hasLocalStart();
    if (!hasLocalStart && link === 'offline' && this.src.low.session.currentInstance() === null) {
      const why = linkProblem(s);
      return new ActionError('offline', why?.message ?? 'offline', 'refused');
    }
    if (!hasLocalStart && s.allowed?.start.reason === 'starting') {
      return new ActionError('starting', 'wait until the new run can be identified', 'refused');
    }
    if (!this.isStoppable(s)) {
      const message =
        s.allowed?.stop.reason === 'not-running' && s.allowed.stop.message
          ? s.allowed.stop.message
          : 'no run is going: there is nothing to stop';
      return new ActionError('not-running', message, 'refused');
    }
    if (!hasLocalStart && this.src.low.session.currentTarget() === null) {
      return new ActionError('not-running', 'no run is going: there is nothing to stop', 'refused');
    }
    return null;
  }

  stop(): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', 'the client was closed before stop was sent', 'refused'),
      );
    }
    if (this.needsSyncForStop()) {
      if (this.syncingStop) return this.syncingStop;
      const pending = this.waitForSync('stop', true, () => !this.needsSyncForStop()).then(
        () => {
          if (this.syncingStop === pending) this.syncingStop = null;
          return this.stopReady();
        },
        (error: unknown) => {
          if (this.syncingStop === pending) this.syncingStop = null;
          throw error;
        },
      );
      this.syncingStop = pending;
      return pending;
    }
    return this.stopReady();
  }

  private stopReady(): Promise<void> {
    const refusal = this.refusalBeforeStop(this.src.snapshot(), 'stop');
    if (refusal) return refusal.code === 'closed' ? Promise.reject(refusal) : this.fail(refusal);
    return this.once('stop', false);
  }

  forceStop(target?: RunTarget): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', 'the client was closed before force_stop was sent', 'refused'),
      );
    }
    if (this.src.low.run.starting.get())
      return this.fail(
        new ActionError('starting', 'wait until the new run can be identified', 'refused'),
      );
    if (this.needsSyncForStop()) {
      if (!target && this.syncingForceStop) return this.syncingForceStop;
      const pending = this.waitForSync('force_stop', true, () => !this.needsSyncForStop()).then(
        () => {
          if (this.syncingForceStop === pending) this.syncingForceStop = null;
          return this.forceStopReady(target);
        },
        (error: unknown) => {
          if (this.syncingForceStop === pending) this.syncingForceStop = null;
          throw error;
        },
      );
      if (!target) this.syncingForceStop = pending;
      return pending;
    }
    return this.forceStopReady(target);
  }

  private forceStopReady(target?: RunTarget): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', 'the client was closed before force_stop was sent', 'refused'),
      );
    }
    if (this.src.low.run.starting.get())
      return this.fail(
        new ActionError('starting', 'wait until the new run can be identified', 'refused'),
      );
    const current = this.src.low.session.currentTarget();
    const active = this.src.activeTarget?.() ?? current;
    const startedId = this.startedRunIdValue.get();
    const effectiveRunId = startedId ?? active?.run_id ?? null;
    const effectiveInstance = active?.instance ?? current?.instance;
    if (target && (effectiveRunId !== target.run_id || effectiveInstance !== target.instance))
      return this.fail(
        new ActionError('session-changed', 'the confirmed run has changed', 'refused'),
      );
    const refusal = this.refusalBeforeStop(this.src.snapshot(), 'force_stop');
    if (refusal) return refusal.code === 'closed' ? Promise.reject(refusal) : this.fail(refusal);
    return this.once('force_stop', true);
  }

  /** Pending stop only affects controls of the run it was sent for. */
  busyForCurrent(): ReadonlySet<OperationName> {
    const busy = new Set(this.busyValue.get());
    const key = this.stopKey();
    for (const name of ['stop', 'force_stop'] as const) {
      if (this.inFlight.get(name)?.key !== key) busy.delete(name);
    }
    return busy;
  }

  private stopKey(): object | string {
    return this.src.captureStop().key;
  }

  /** Repeated requests for the same run share one operation. */
  private once(name: 'stop' | 'force_stop', force: boolean): Promise<void> {
    const capture = this.src.captureStop();
    const key = capture.key;
    const running = this.inFlight.get(name);
    if (running?.key === key) return running.promise;
    let resolve!: () => void;
    let reject!: (reason: unknown) => void;
    const promise = new Promise<void>((ok, fail) => {
      resolve = ok;
      reject = fail;
    });
    let finished = false;
    const entry = {
      key,
      promise,
      cancel: () => {
        if (finished) return;
        finished = true;
        if (this.inFlight.get(name) === entry) {
          this.inFlight.delete(name);
          this.setBusy(name, false);
        }
        reject(
          this.record(
            new ActionError('closed', `the client was closed before ${name} was done`, 'unknown'),
          ),
        );
      },
    };
    this.inFlight.set(name, entry);
    this.failureValue.set(null);
    // The low-level method snapshots the target before any Front busy notification.
    const operation = capture.send(force);
    if (this.busyValue.get().has(name)) this.busyValue.set(new Set(this.busyValue.get()));
    else this.setBusy(name, true);
    operation.then(
      (ack) => {
        if (finished) return;
        finished = true;
        this.stopConfirmedForValue.set(ack.run_id);
        if (this.inFlight.get(name) === entry) {
          this.inFlight.delete(name);
          this.setBusy(name, false);
        }
        resolve();
      },
      (error: unknown) => {
        if (finished) return;
        finished = true;
        if (this.inFlight.get(name) === entry) {
          this.inFlight.delete(name);
          this.setBusy(name, false);
        }
        reject(this.record(actionError(error)));
      },
    );
    return promise;
  }

  private guarded(name: OperationName, send: () => Promise<void>): Promise<void> {
    this.failureValue.set(null);
    this.setBusy(name, true);
    return send().then(
      () => this.setBusy(name, false),
      (error: unknown) => {
        this.setBusy(name, false);
        throw this.record(actionError(error));
      },
    );
  }

  // --- time --------------------------------------------------------------------------------------

  pause(target?: RunTarget | null): Promise<void> {
    return this.time('pause', { command: 'pause' }, 0, target);
  }

  resume(target?: RunTarget | null): Promise<void> {
    return this.time('resume', { command: 'resume' }, 0, target);
  }

  step(ms: number, target?: RunTarget | null): Promise<void> {
    if (!Number.isFinite(ms) || ms <= 0) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          'step needs a number of milliseconds greater than 0',
          'refused',
        ),
      );
    }
    return this.time('step', { command: 'step', ms }, ms, target);
  }

  setSpeed(value: number | 'max', target?: RunTarget | null): Promise<void> {
    if (value !== 'max' && !(Number.isFinite(value) && value > 0)) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          "the speed is a number greater than 0, or 'max'",
          'refused',
        ),
      );
    }
    return this.time('speed', { command: 'speed', value }, 0, target);
  }

  runUntil(ms: number, target?: RunTarget | null): Promise<void> {
    if (!Number.isFinite(ms) || ms < 0) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          'run_until needs a time in milliseconds, 0 or more',
          'refused',
        ),
      );
    }
    return this.time('run_until', { command: 'run_until', at_ms: ms }, ms, target);
  }

  private time(
    op: TimeOperation,
    body: Record<string, unknown>,
    amount: number,
    shownTarget?: RunTarget | null,
  ): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', `the client was closed before ${op} was sent`, 'refused'),
      );
    }
    if (linkOf(this.src.snapshot()) === 'connecting') {
      return this.waitForSync(op).then(() => this.timeReady(op, body, amount, shownTarget));
    }
    return this.timeReady(op, body, amount, shownTarget);
  }

  private timeReady(
    op: TimeOperation,
    body: Record<string, unknown>,
    amount: number,
    shownTarget?: RunTarget | null,
  ): Promise<void> {
    if (this.disposed) {
      return Promise.reject(
        new ActionError('closed', `the client was closed before ${op} was sent`, 'refused'),
      );
    }
    const target = this.src.low.session.currentTarget();
    this.failureValue.set(null);
    const snapshot = this.src.snapshot();
    const link = linkOf(snapshot);
    // The client refuses only what it owns: the connection, and the same operation still waiting for its answer
    const own =
      link === 'rejected' || link === 'incompatible'
        ? this.src.controls()[op].reason
        : link === 'offline' && this.src.low.session.currentInstance() === null
          ? linkProblem(snapshot)
          : this.busyValue.get().has(op)
            ? { code: 'busy', message: `the previous ${op} has not been answered yet: wait for it` }
            : null;
    if (own) return this.fail(new ActionError(own.code, own.message, 'refused'));
    if (shownTarget !== undefined && !sameRunTarget(shownTarget, target))
      return this.fail(
        new ActionError('session-changed', 'the displayed run has changed', 'refused'),
      );
    if (!target)
      return this.fail(new ActionError('not-running', 'there is no identified run', 'refused'));
    // Already as wanted: nothing to send (the pause button of a second tab does not turn up as a mistake). `controls`
    // gives these two codes only when the copy can be trusted: online, with a run that takes the operation
    const code = this.src.controls()[op === 'pause' ? 'pause' : 'resume'].reason?.code;
    if ((op === 'pause' && code === 'paused') || (op === 'resume' && code === 'not-paused')) {
      return Promise.resolve();
    }
    const id = this.nextId++;
    return new Promise<void>((resolve, reject) => {
      const waiting: Waiting = {
        id,
        op,
        amount,
        ...target,
        stage: 'sent',
        target: 0,
        from: 0,
        exact: false,
        timer: undefined,
        resolve,
        reject,
      };
      this.waiting.set(id, waiting);
      this.setBusy(op, true);
      waiting.timer = setTimeout(() => {
        this.settle(
          waiting,
          new ActionError(
            'no-answer',
            `the launcher did not answer ${op} within ${this.src.answerTimeoutMs / 1000} s (it may not read its input, or the answer went to another command): look at status and clock before you send it again`,
            'unknown',
          ),
        );
      }, this.timeoutDelayMs());
      this.src.http(
        { method: 'POST', path: 'api/time', body: { ...body, id, ...target } },
        (outcome) => {
          if (!outcome.ok) this.settle(waiting, actionError(outcome.error));
        },
      );
    });
  }

  /** Ends a waiting command: with an error, or (null) as done. A command ended twice is ended once. */
  private settle(waiting: Waiting, error: ActionError | null, report = true): void {
    if (this.waiting.get(waiting.id) !== waiting) return;
    clearTimeout(waiting.timer);
    this.waiting.delete(waiting.id);
    if (waiting.stage === 'sent') this.setBusy(waiting.op, this.stillSent(waiting.op));
    if (error) {
      if (report) this.record(error);
      waiting.reject(error);
    } else waiting.resolve();
  }

  private stillSent(op: TimeOperation): boolean {
    for (const w of this.waiting.values()) if (w.op === op && w.stage === 'sent') return true;
    return false;
  }

  private setBusy(name: OperationName, on: boolean): void {
    const now = this.busyValue.get();
    if (now.has(name) === on) return;
    const next = new Set(now);
    if (on) next.add(name);
    else next.delete(name);
    this.busyValue.set(next);
  }

  /** The launcher's events: an answer to a command of ours, and commands of other senders. */
  private onMessages(batch: readonly ServerMessage[]): void {
    let sim = this.lastSim;
    for (const message of batch) {
      if (message.type !== 'event') continue;
      const event: LauncherEvent = message.event;
      if (event.event === 'time') sim = event.sim_ms;
      if (event.event !== 'command') continue;
      const mine = event.id === null ? undefined : this.waiting.get(event.id);
      if (mine && mine.run_id === message.run_id) this.answered(mine, event, sim);
      else if (event.accepted && event.command !== null && MOVES.has(event.command)) {
        // Another sender's command: a step on its way no longer ends where it was to
        this.interruptMoving(
          null,
          'another command (from another page or from stdin) came before it arrived',
        );
      }
    }
    this.lastSim = this.src.low.session.session.get()?.time?.simMs ?? sim;
    this.evaluate();
  }

  private answered(
    waiting: Waiting,
    event: Extract<LauncherEvent, { event: 'command' }>,
    sim: number | null,
  ): void {
    if (!event.accepted) {
      this.settle(
        waiting,
        new ActionError(
          event.reason ?? 'refused',
          event.message ?? `the launcher did not accept ${waiting.op}`,
          'refused',
        ),
      );
      return;
    }
    if (waiting.op === 'pause' || waiting.op === 'resume') {
      // This page's own pause or resume: the step that it ends is not a problem to show
      this.interruptMoving(waiting, `${waiting.op} of this page came before it arrived`, false);
    }
    if (waiting.op === 'step' || waiting.op === 'run_until') {
      clearTimeout(waiting.timer);
      waiting.stage = 'moving';
      waiting.from = sim ?? 0;
      // A step ends at the time it was accepted plus the amount the launcher rounded it to; a run_until at its time
      // (the launcher's own `at_ms` says it exactly, and is used when it is there)
      waiting.exact = event.at_ms !== null;
      waiting.target =
        event.at_ms ??
        (waiting.op === 'step' ? waiting.from + (event.ms ?? waiting.amount) : waiting.amount);
      this.setBusy(waiting.op, this.stillSent(waiting.op));
      return;
    }
    this.settle(waiting, null);
  }

  /** Ends the steps and run_untils on their way (except `keep`) as interrupted. */
  private interruptMoving(keep: Waiting | null, why: string, report = true): void {
    for (const w of [...this.waiting.values()]) {
      if (w !== keep && w.stage === 'moving') {
        this.settle(
          w,
          new ActionError(
            'interrupted',
            `${w.op} did not arrive: ${why}; read clock to see where the time is`,
            'unknown',
          ),
          report,
        );
      }
    }
  }

  /** Ends what the state of the run decides: the run is over, time has arrived, or a stop is on its way. */
  private evaluate(): void {
    if (this.waiting.size === 0) return;
    const session = this.src.low.session.session.get();
    const instance = this.src.low.session.currentInstance();
    for (const w of [...this.waiting.values()]) {
      if (
        !session ||
        !isRunActive(session) ||
        session.id !== w.run_id ||
        instance !== w.instance ||
        session.result
      ) {
        this.settle(
          w,
          new ActionError(
            'not-running',
            `the run ended (or another one began) before ${w.op} was done: look at status`,
            'unknown',
          ),
        );
        continue;
      }
      if (w.stage !== 'moving') continue;
      const state = session.launcherState;
      if ((state?.stop?.requests ?? 0) > 0 || session.stopRequests > 0) {
        this.settle(
          w,
          new ActionError('interrupted', `${w.op} did not arrive: a stop was asked for`, 'unknown'),
          false, // a stop by the operator is not a problem to show
        );
        continue;
      }
      const stands = standsAt(session);
      // Arrived: the latest state says that it stands still, at or after the target
      if (stands !== null && (w.exact || stands > w.from) && stands >= w.target)
        this.settle(w, null);
    }
  }

  // --- bookkeeping -------------------------------------------------------------------------------

  /** Keeps the error for `status.problem`. */
  record(error: ActionError): ActionError {
    this.failureValue.set({ error, run: this.src.run(), stamp: this.src.stamp() });
    return error;
  }

  private fail(error: ActionError): Promise<void> {
    return Promise.reject(this.record(error));
  }

  /** Stops waiting (the client closes): what is waiting ends as unknown, and a check that waits is not sent. */
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.close(closedCheck());
    for (const cancel of [...this.syncCancelers]) cancel();
    for (const stop of this.stops) stop();
    for (const [, entry] of [...this.inFlight]) {
      entry.cancel();
    }
    for (const w of [...this.waiting.values()]) {
      this.settle(
        w,
        new ActionError('closed', `the client was closed before ${w.op} was done`, 'unknown'),
      );
    }
  }
}
