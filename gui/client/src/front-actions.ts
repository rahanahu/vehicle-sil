// The Promise operations of the front door. A time command carries an `id` that this page chose, and its answer is the
// launcher's `command` event with the same `id`; the answer timeout belongs to this file (not to `client.time`, which
// stops waiting when any command event arrives). `step` and `runUntil` resolve when time has arrived and the run is
// paused again.
import type { GuiClient } from './client';
import { HttpError, HttpTransport, errorMessage } from './http';
import type { FrontControls, Intent, OperationName, RunState } from './front-types';
import { ActionError } from './front-types';
import type { Snapshot } from './front-compose';
import { linkOf } from './front-compose';
import type { LauncherEvent, RunRequest, ServerMessage } from './protocol';
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
  session: number;
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

/**
 * The commands that make a step on its way end somewhere else: a pause or a resume from another sender. (A step or a
 * run_until sent meanwhile does not: the launcher carries the first one on and counts the next from where it stops.)
 */
const MOVES = new Set(['pause', 'resume']);

/** The fields of an `Intent` (what `run.start()` takes). */
const INTENT_FIELDS = new Set([
  'disable',
  'manual',
  'scenario',
  'variant',
  'params',
  'speed',
  'start_paused',
]);

export class FrontActions {
  private readonly busyValue: Writable<ReadonlySet<OperationName>>;
  private readonly failureValue: Writable<Failure | null>;
  private readonly waiting = new Map<number, Waiting>();
  private readonly inFlight = new Map<'stop' | 'force_stop', Promise<void>>();
  private nextId: number;
  private lastSim: number | null = null;
  private readonly stops: (() => void)[] = [];
  private disposed = false;

  readonly busy: Readable<ReadonlySet<OperationName>>;
  readonly failure: Readable<Failure | null>;

  constructor(
    private readonly src: ActionSources,
    /** Where the ids start (they must differ between pages that send to the same run). */
    idStart: number = 1 + Math.floor(Math.random() * 2 ** 40),
  ) {
    this.nextId = idStart;
    this.busyValue = new Writable<ReadonlySet<OperationName>>(new Set(), Object.is, src.onError);
    this.failureValue = new Writable<Failure | null>(null, Object.is, src.onError);
    this.busy = this.busyValue.readonly();
    this.failure = this.failureValue.readonly();
    this.stops.push(
      src.low.session.onMessages((batch) => this.onMessages(batch)),
      src.low.session.session.subscribe(() => this.evaluate()),
    );
  }

  // --- run ---------------------------------------------------------------------------------------

  start(intent: Intent = {}, options: { file?: string } = {}): Promise<void> {
    const file = options.file ?? this.src.initialFile();
    if (file === null) {
      return this.fail(
        new ActionError(
          'no-file',
          'no configuration file was given and the server has none to start with: pass { file } (a path from client.low.configs())',
          'refused',
        ),
      );
    }
    // A field the client does not know (a misspelt `scenaro` from plain JavaScript) would be dropped and the run
    // started without it: refuse it instead.
    const unknown = Object.keys(intent).filter((k) => !INTENT_FIELDS.has(k));
    if (unknown.length > 0) {
      return this.fail(
        new ActionError(
          'unknown-field',
          `start does not know ${unknown.join(', ')}: the fields are ${[...INTENT_FIELDS].join(', ')}`,
          'refused',
        ),
      );
    }
    const own = this.refusalOfStart();
    if (own) return this.fail(own);
    const request: RunRequest = {
      file,
      disable: intent.disable ?? [],
      speed: intent.speed ?? null,
      start_paused: intent.start_paused ?? false,
      ...(intent.manual ? { manual: intent.manual } : {}),
      ...(intent.scenario !== undefined ? { scenario: intent.scenario } : {}),
      ...(intent.variant !== undefined ? { variant: intent.variant } : {}),
      ...(intent.params ? { params: intent.params } : {}),
    };
    return this.guarded('start', () => this.src.low.run.start(request).then(() => undefined));
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

  stop(): Promise<void> {
    return this.once('stop', () => this.src.low.run.stop());
  }

  forceStop(): Promise<void> {
    return this.once('force_stop', () => this.src.low.run.forceStop());
  }

  /** A stop asked again while its request is on its way is the same request (a double click sends nothing more). */
  private once(name: 'stop' | 'force_stop', send: () => Promise<unknown>): Promise<void> {
    const running = this.inFlight.get(name);
    if (running) return running;
    // The stop goes out whatever the connection says: HTTP may reach the server when the WebSocket is down
    const promise = this.guarded(name, () => send().then(() => undefined)).finally(() => {
      this.inFlight.delete(name);
    });
    this.inFlight.set(name, promise);
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

  pause(): Promise<void> {
    return this.time('pause', { command: 'pause' }, 0);
  }

  resume(): Promise<void> {
    return this.time('resume', { command: 'resume' }, 0);
  }

  step(ms: number): Promise<void> {
    if (!Number.isFinite(ms) || ms <= 0) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          'step needs a number of milliseconds greater than 0',
          'refused',
        ),
      );
    }
    return this.time('step', { command: 'step', ms }, ms);
  }

  setSpeed(value: number | 'max'): Promise<void> {
    if (value !== 'max' && !(Number.isFinite(value) && value > 0)) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          "the speed is a number greater than 0, or 'max'",
          'refused',
        ),
      );
    }
    return this.time('speed', { command: 'speed', value }, 0);
  }

  runUntil(ms: number): Promise<void> {
    if (!Number.isFinite(ms) || ms < 0) {
      return this.fail(
        new ActionError(
          'invalid-argument',
          'run_until needs a time in milliseconds, 0 or more',
          'refused',
        ),
      );
    }
    return this.time('run_until', { command: 'run_until', at_ms: ms }, ms);
  }

  private time(op: TimeOperation, body: Record<string, unknown>, amount: number): Promise<void> {
    this.failureValue.set(null);
    const snapshot = this.src.snapshot();
    const link = linkOf(snapshot);
    // The client refuses only what it owns: the connection, and the same operation still waiting for its answer
    const own =
      link === 'rejected' || link === 'incompatible'
        ? this.src.controls()[op].reason
        : this.busyValue.get().has(op)
          ? { code: 'busy', message: `the previous ${op} has not been answered yet: wait for it` }
          : null;
    if (own) return this.fail(new ActionError(own.code, own.message, 'refused'));
    // Already as wanted: nothing to send (the pause button of a second tab does not turn up as a mistake). `controls`
    // gives these two codes only when the copy can be trusted: online, with a run that takes the operation
    const code = this.src.controls()[op === 'pause' ? 'pause' : 'resume'].reason?.code;
    if ((op === 'pause' && code === 'paused') || (op === 'resume' && code === 'not-paused')) {
      return Promise.resolve();
    }
    const session = snapshot.session;
    const id = this.nextId++;
    return new Promise<void>((resolve, reject) => {
      const waiting: Waiting = {
        id,
        op,
        amount,
        session: session?.id ?? -1,
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
      }, this.src.answerTimeoutMs);
      this.src.http({ method: 'POST', path: 'api/time', body: { ...body, id } }, (outcome) => {
        if (!outcome.ok) this.settle(waiting, actionError(outcome.error));
      });
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
      if (mine && mine.session === message.session) this.answered(mine, event, sim);
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
    for (const w of [...this.waiting.values()]) {
      if (!session || session.id !== w.session || !session.launcher.running || session.result) {
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
      const sim = session.time?.simMs ?? session.simMs;
      const paused = state ? state.paused : session.time?.paused;
      if (sim !== null && (w.exact || sim > w.from) && sim >= w.target && paused === true)
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

  /** Stops waiting (the client closes): what is waiting ends as unknown. */
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const stop of this.stops) stop();
    for (const w of [...this.waiting.values()]) {
      this.settle(
        w,
        new ActionError('closed', `the client was closed before ${w.op} was done`, 'unknown'),
      );
    }
  }
}
