import { HttpError, HttpTransport, errorMessage } from './http';
import {
  ProtocolError,
  RunAck,
  RunRequest,
  readRunResponse,
  readSessionInstance,
} from './protocol';
import { RunTarget, SessionFeed } from './session';
import { isRunActive } from './session-state';
import { ErrorSink, Readable, Writable, derived, rethrowLater } from './values';

/** Stop action: stop in order, or stop without waiting. */
export type StopAction = 'stop' | 'force';

/**
 * Text for an action that got no answer (timed out). It was not refused, so the server may have carried it out: for
 * a start, the WebSocket state (the session copy) tells whether the run started. null unless no answer came.
 */
function answerMissing(path: string, error: HttpError): string | null {
  if (!error.timedOut) return null;
  return path === 'api/run'
    ? `${error.message}: the server may still start the run (it checks the file again when it has changed); ` +
        'whether it did shows in the session over the WebSocket'
    : `${error.message}: the server may have done it; the session over the WebSocket shows what happened`;
}

/** Why an action was refused, and the run at that time (once the run changes, the old reason is no longer shown). */
interface ActionFailure {
  message: string;
  session: string | null;
  running: boolean;
}

export interface StopCapture {
  readonly key: object | string;
  readonly send: (force: boolean) => Promise<RunAck>;
}

/** Run control and its state (GuiClient.run). */
export interface ClientRun {
  /** A start was sent and its answer is pending. */
  readonly starting: Readable<boolean>;
  /** A stop was sent and its answer is pending (which kind of stop). */
  readonly stopping: Readable<StopAction | null>;
  /** A pending stop of the current session or current start, if any. */
  readonly currentStopping: Readable<StopAction | null>;
  /** A stop was asked for while the answer to a start was pending. */
  readonly stopWhileStarting: Readable<boolean>;
  /** Why the last action was refused (the server's text, unchanged). null once a run starts or ends. */
  readonly error: Readable<string | null>;
  /**
   * Starts a run (`POST api/run`). Resolves with the `{instance, run_id}` acknowledgement; rejects with an HttpError
   * if refused. The server may check the configuration file again as part of the start, so this waits up to twice the
   * check limit (waiting its turn, then checking) plus a margin. If there is still no answer, it rejects with an
   * HttpError with `timedOut` and error says that no answer came (the WebSocket state tells whether the run started).
   */
  start(request: RunRequest): Promise<RunAck>;
  /**
   * Stops the session currently seen by this client (`POST api/stop-session`). During its own pending start, waits for
   * the answer and targets that session. If the ID or server instance is unknown, or the start answer is lost, rejects
   * without sending an unscoped stop.
   */
  stop(): Promise<RunAck>;
  /** Stops without waiting for participants. A stop during this client's pending start is targeted the same way. */
  forceStop(): Promise<RunAck>;
}

/**
 * Run control: start (`POST api/run`), stop in order or without waiting (`POST api/stop-session`), and their state.
 *
 * - HTTP commands return `{instance, run_id}` acknowledgements; state transitions arrive over the single WebSocket
 *   stream.
 * - A stop requested while a start is pending waits for its answer, then names that `run_id` and `instance` in the
 *   stop request. The server checks the ID atomically, so a later run cannot be stopped by the delayed request.
 * - error holds the reason for a refusal (the server's English text). It is cleared once a run starts or ends.
 */
export class RunControl implements ClientRun {
  private startOperation: Promise<RunAck> | null = null;
  private startedTarget: { target: RunTarget; sessionBefore: unknown } | null = null;
  private readonly pendingStops = new Set<{
    action: StopAction;
    starting: boolean;
    key: object | string;
  }>();
  private readonly pendingRevision: Writable<number>;
  private readonly startSending: Writable<boolean>;
  private readonly stopSending: Writable<StopAction | null>;
  private readonly stopAskedWhileStarting: Writable<boolean>;
  private readonly failure: Writable<ActionFailure | null>;

  readonly starting: Readable<boolean>;
  readonly stopping: Readable<StopAction | null>;
  readonly currentStopping: Readable<StopAction | null>;
  readonly stopWhileStarting: Readable<boolean>;
  readonly error: Readable<string | null>;

  constructor(
    private readonly feed: SessionFeed,
    private readonly transport: HttpTransport,
    onError: ErrorSink = rethrowLater,
    /**
     * How long to wait for the answer to a start (ms). Long, because the server may check again as part of the start.
     */
    private readonly startTimeoutMs: () => number = () => 0,
  ) {
    this.startSending = new Writable<boolean>(false, Object.is, onError);
    this.stopSending = new Writable<StopAction | null>(null, Object.is, onError);
    this.pendingRevision = new Writable<number>(0, Object.is, onError);
    this.stopAskedWhileStarting = new Writable<boolean>(false, Object.is, onError);
    this.failure = new Writable<ActionFailure | null>(null, Object.is, onError);
    this.starting = this.startSending.readonly();
    this.stopping = this.stopSending.readonly();
    this.currentStopping = derived(
      [this.pendingRevision, this.startSending, feed.session],
      () => {
        const key = this.startSending.get()
          ? this.startOperation
          : JSON.stringify(this.activeTarget());
        return (
          [...this.pendingStops].filter((pending) => pending.key === key).at(-1)?.action ?? null
        );
      },
      Object.is,
      onError,
    );
    this.stopWhileStarting = this.stopAskedWhileStarting.readonly();
    feed.session.subscribe(() => {
      this.startedTarget = null;
    });
    this.error = derived(
      [this.failure, feed.session],
      () => {
        const failure = this.failure.get();
        const session = feed.session.get();
        if (!failure) return null;
        const same =
          failure.session === (session?.id ?? null) && failure.running === isRunActive(session);
        return same ? failure.message : null;
      },
      Object.is,
      onError,
    );
  }

  /** Starts a run. */
  start(request: RunRequest): Promise<RunAck> {
    if (this.startSending.get()) return Promise.reject(new Error('a start is already pending'));
    let resolveStart!: (result: RunAck) => void;
    let rejectStart!: (reason: unknown) => void;
    const operation = new Promise<RunAck>((resolve, reject) => {
      resolveStart = resolve;
      rejectStart = reject;
    });
    // Publish the operation before notifying subscribers of `starting`: one of them can ask to stop immediately.
    this.startOperation = operation;
    this.startedTarget = null;
    this.stopAskedWhileStarting.set(false);
    this.startSending.set(true);
    this.send('api/run', request, this.startTimeoutMs() || undefined, (ack) => {
      if (ack && this.feed.currentTarget()?.run_id !== ack.run_id) {
        this.startedTarget = {
          target: { run_id: ack.run_id, instance: ack.instance },
          sessionBefore: this.feed.session.get(),
        };
      }
      this.startSending.set(false);
    }).then(resolveStart, rejectStart);
    return operation;
  }

  activeTarget(): RunTarget | null {
    if (this.startedTarget && this.feed.session.get() === this.startedTarget.sessionBefore) {
      return this.startedTarget.target;
    }
    return this.feed.currentTarget();
  }

  /** Stops in order (waits for the participants to end). Asking again never turns it into a stop without waiting. */
  stop(): Promise<RunAck> {
    return this.requestStop(false);
  }

  /** Stops without waiting (kills the participants). */
  forceStop(): Promise<RunAck> {
    return this.requestStop(true);
  }

  captureStop(): StopCapture {
    const starting = this.startSending.get() ? this.startOperation : null;
    const target = starting ? null : this.activeTarget();
    return {
      key: starting ?? JSON.stringify(target),
      send: (force) => this.requestStop(force, starting, target),
    };
  }

  private requestStop(
    force: boolean,
    starting = this.startSending.get() ? this.startOperation : null,
    target: RunTarget | null = starting ? null : this.activeTarget(),
  ): Promise<RunAck> {
    const session = this.feed.session.get();
    const pending = {
      action: force ? ('force' as const) : ('stop' as const),
      starting: !!starting,
      key: starting ?? JSON.stringify(target),
    };
    this.pendingStops.add(pending);
    this.pendingRevision.update((n) => n + 1);
    this.stopSending.set(pending.action);
    const done = () => {
      this.pendingStops.delete(pending);
      this.pendingRevision.update((n) => n + 1);
      const remaining = [...this.pendingStops];
      this.stopAskedWhileStarting.set(remaining.some((p) => p.starting));
      this.stopSending.set(remaining.at(-1)?.action ?? null);
    };
    if (!starting) {
      if (!target) {
        const message =
          'no session and server instance are known: no stop was sent; reconnect and check the current run';
        this.failure.set({
          message,
          session: session?.id ?? null,
          running: isRunActive(session),
        });
        done();
        return Promise.reject(new Error(message));
      }
      return this.send('api/stop-session', { force, ...target }, undefined, done);
    }
    this.stopAskedWhileStarting.set(true);
    return starting
      .then(
        (ack) => {
          pending.key = JSON.stringify({ run_id: ack.run_id, instance: ack.instance });
          this.pendingRevision.update((n) => n + 1);
          return this.send(
            'api/stop-session',
            { force, run_id: ack.run_id, instance: ack.instance },
            undefined,
            () => undefined,
          );
        },
        (error: unknown) => {
          const session = this.feed.session.get();
          this.failure.set({
            message: `the start was not confirmed; no stop was sent: ${errorMessage(error)}`,
            session: session?.id ?? null,
            running: isRunActive(session),
          });
          throw new Error(`the start was not confirmed; no stop was sent: ${errorMessage(error)}`);
        },
      )
      .finally(done);
  }

  /**
   * Sends an HTTP command and reads its `{instance, run_id}` acknowledgement. done is called after the answer
   * (failures too), before the Promise settles.
   */
  private send(
    path: string,
    body: object,
    timeoutMs: number | undefined,
    done: (ack?: RunAck) => void,
  ): Promise<RunAck> {
    const incompatible = this.feed.incompatible.get();
    if (incompatible) {
      done();
      return Promise.reject(new Error(incompatible));
    }
    this.failure.set(null);
    return new Promise((resolve, reject) => {
      this.transport({ method: 'POST', path, body, timeoutMs }, (outcome) => {
        if (!outcome.ok) {
          const failure =
            path === 'api/stop-session' && outcome.error.status === 404
              ? new HttpError(404, outcome.error.statusText, {
                  code: 'targeted-stop-unsupported',
                  message:
                    'the GUI server cannot stop a particular session: no stop was sent; use the GUI server of this release',
                })
              : outcome.error;
          const session = this.feed.session.get();
          this.failure.set({
            message: answerMissing(path, failure) ?? errorMessage(failure),
            session: session?.id ?? null,
            running: isRunActive(session),
          });
          done();
          reject(failure);
          return;
        }
        let ack: RunAck;
        try {
          if (path === 'api/stop-session') {
            const target = body as RunTarget;
            const raw =
              typeof outcome.body === 'object' && outcome.body !== null
                ? (outcome.body as Record<string, unknown>)
                : {};
            ack = readRunResponse({
              instance: raw['instance'] ?? target.instance,
              run_id: raw['run_id'],
            });
            if (ack.run_id !== target.run_id || ack.instance !== target.instance) {
              throw new Error('the stop answer did not identify the requested session');
            }
          } else {
            readSessionInstance(outcome.body);
            ack = readRunResponse(outcome.body);
          }
        } catch (error) {
          this.feed.report(
            error instanceof ProtocolError ? `unreadable answer: ${error.message}` : String(error),
          );
          const missingInstance =
            error instanceof ProtocolError && error.path === 'response.instance';
          const message = missingInstance
            ? `${error.message}. The run may have started. Check the current run before retrying`
            : path === 'api/stop-session'
              ? 'the stop answer was unreadable: check the session before trying again'
              : 'the start answer did not identify the run; no stop was sent';
          const session = this.feed.session.get();
          this.failure.set({
            message,
            session: session?.id ?? null,
            running: isRunActive(session),
          });
          done();
          reject(new Error(message));
          return;
        }
        done(ack);
        resolve(ack);
      });
    });
  }
}
