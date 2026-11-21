import { HttpError, HttpTransport, errorMessage } from './http';
import { ProtocolError, RunRequest, SessionSummary, readSessionResponse } from './protocol';
import { SessionFeed } from './session';
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
  session: number | null;
  running: boolean;
}

export interface StopCapture {
  readonly key: object | string;
  readonly send: (force: boolean) => Promise<SessionSummary | null>;
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
   * Starts a run (`POST api/run`). Resolves with the summary in the answer; rejects with an HttpError if refused.
   * The server may check the configuration file again as part of the start, so this waits up to twice the check limit
   * (waiting its turn, then checking) plus a margin. If there is still no answer, it rejects with an HttpError with
   * `timedOut` and error says that no answer came (the WebSocket state tells whether the run started). The server may
   * have started it.
   */
  start(request: RunRequest): Promise<SessionSummary | null>;
  /**
   * Stops the session currently seen by this client. During its own pending start, waits for the answer and targets
   * that session. If the ID or server instance is unknown, or the start answer is lost, rejects without sending an
   * unscoped stop. An older server without `api/stop-session` is refused, rather than stopping an unrelated run.
   */
  stop(): Promise<SessionSummary | null>;
  /** Stops without waiting for participants. A stop during this client's pending start is targeted the same way. */
  forceStop(): Promise<SessionSummary | null>;
}

/**
 * Run control: start (`POST api/run`), stop in order or without waiting (`POST api/stop-session`), and their state.
 *
 * - The answer to an action (the current summary) is applied to the session copy without waiting for the notification
 *   (no stale display right after a click).
 * - A stop requested while a start is pending waits for its answer, then names that session in the stop request. The
 *   server checks the ID atomically, so a later run cannot be stopped by the delayed request.
 * - error holds the reason for a refusal (the server's English text). It is cleared once a run starts or ends.
 * - The Promises of start() and stop() resolve with the summary in the answer (null if unreadable) and reject with an
 *   HttpError when refused. No answer (`timedOut`) is not a refusal: the session copy tells whether the run started.
 *   The state (starting and so on) has already changed when the Promise resolves.
 */
export class RunControl implements ClientRun {
  private startOperation: Promise<{
    summary: SessionSummary | null;
    instance: string | null;
  }> | null = null;
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
          : JSON.stringify(feed.currentTarget());
        return (
          [...this.pendingStops].filter((pending) => pending.key === key).at(-1)?.action ?? null
        );
      },
      Object.is,
      onError,
    );
    this.stopWhileStarting = this.stopAskedWhileStarting.readonly();
    this.error = derived(
      [this.failure, feed.session],
      () => {
        const failure = this.failure.get();
        const session = feed.session.get();
        if (!failure) return null;
        const same =
          failure.session === (session?.id ?? null) &&
          failure.running === (session?.launcher.running ?? false);
        return same ? failure.message : null;
      },
      Object.is,
      onError,
    );
  }

  /** Starts a run. */
  start(request: RunRequest): Promise<SessionSummary | null> {
    if (this.startSending.get()) return Promise.reject(new Error('a start is already pending'));
    let resolveStart!: (result: {
      summary: SessionSummary | null;
      instance: string | null;
    }) => void;
    let rejectStart!: (reason: unknown) => void;
    const operation = new Promise<{ summary: SessionSummary | null; instance: string | null }>(
      (resolve, reject) => {
        resolveStart = resolve;
        rejectStart = reject;
      },
    );
    // Publish the operation before notifying subscribers of `starting`: one of them can ask to stop immediately.
    this.startOperation = operation;
    this.stopAskedWhileStarting.set(false);
    this.startSending.set(true);
    let instance: string | null = null;
    this.send(
      'api/run',
      request,
      this.startTimeoutMs() || undefined,
      () => {
        this.startSending.set(false);
      },
      (seen) => {
        instance = seen;
      },
    ).then((summary) => resolveStart({ summary, instance }), rejectStart);
    return operation.then(({ summary }) => summary);
  }

  /** Stops in order (waits for the participants to end). Asking again never turns it into a stop without waiting. */
  stop(): Promise<SessionSummary | null> {
    return this.requestStop(false);
  }

  /** Stops without waiting (kills the participants). */
  forceStop(): Promise<SessionSummary | null> {
    return this.requestStop(true);
  }

  captureStop(): StopCapture {
    const starting = this.startSending.get() ? this.startOperation : null;
    const target = starting ? null : this.feed.currentTarget();
    return {
      key: starting ?? JSON.stringify(target),
      send: (force) => this.requestStop(force, starting, target),
    };
  }

  private requestStop(
    force: boolean,
    starting = this.startSending.get() ? this.startOperation : null,
    target = starting ? null : this.feed.currentTarget(),
  ): Promise<SessionSummary | null> {
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
          running: session?.launcher.running ?? false,
        });
        done();
        return Promise.reject(new Error(message));
      }
      return this.send('api/stop-session', { force, ...target }, undefined, done);
    }
    this.stopAskedWhileStarting.set(true);
    return starting
      .then(
        ({ summary, instance }) => {
          if (!summary || !instance) {
            const message = 'the start answer did not identify the run; no stop was sent';
            const session = this.feed.session.get();
            this.failure.set({
              message,
              session: session?.id ?? null,
              running: session?.launcher.running ?? false,
            });
            throw new Error(message);
          }
          pending.key = JSON.stringify({ session: summary.id, instance });
          this.pendingRevision.update((n) => n + 1);
          return this.send(
            'api/stop-session',
            { force, session: summary.id, instance },
            undefined,
            () => undefined,
          );
        },
        (error: unknown) => {
          const message = `the start was not confirmed; no stop was sent: ${errorMessage(error)}`;
          const session = this.feed.session.get();
          this.failure.set({
            message,
            session: session?.id ?? null,
            running: session?.launcher.running ?? false,
          });
          throw new Error(message);
        },
      )
      .finally(done);
  }

  /**
   * Sends and applies the summary in the answer. done is called after the answer (failures too), before the Promise
   * settles.
   */
  private send(
    path: string,
    body: object,
    timeoutMs: number | undefined,
    done: () => void,
    onInstance?: (instance: string | null) => void,
  ): Promise<SessionSummary | null> {
    this.failure.set(null);
    return new Promise((resolve, reject) => {
      const sentFrom = this.feed.currentInstance();
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
            running: session?.launcher.running ?? false,
          });
          done();
          reject(failure);
          return;
        }
        let summary: SessionSummary | null = null;
        try {
          summary = readSessionResponse(outcome.body);
          if (
            path === 'api/stop-session' &&
            (!summary || summary.id !== (body as { session: number }).session)
          ) {
            throw new Error('the stop answer did not identify the requested session');
          }
          const value = (outcome.body as { instance?: unknown } | null)?.instance;
          const instance =
            path === 'api/stop-session'
              ? (body as { instance: string }).instance
              : typeof value === 'string' || value === undefined
                ? value
                : null;
          onInstance?.(instance ?? null);
          this.feed.adopt(summary, instance, instance === undefined ? sentFrom : undefined);
        } catch (error) {
          // An answer whose summary core is unreadable is reported, and the summary of the next notification takes over
          this.feed.report(
            error instanceof ProtocolError ? `unreadable answer: ${error.message}` : String(error),
          );
          if (path === 'api/stop-session') {
            const message = 'the stop answer was unreadable: check the session before trying again';
            const session = this.feed.session.get();
            this.failure.set({
              message,
              session: session?.id ?? null,
              running: session?.launcher.running ?? false,
            });
            done();
            reject(new Error(message));
            return;
          }
        }
        done();
        resolve(summary);
      });
    });
  }
}
