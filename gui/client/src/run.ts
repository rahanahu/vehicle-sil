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

/** Run control and its state (GuiClient.run). */
export interface ClientRun {
  /** A start was sent and its answer is pending. */
  readonly starting: Readable<boolean>;
  /** A stop was sent and its answer is pending (which kind of stop). */
  readonly stopping: Readable<StopAction | null>;
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
  /** Stops in order (`POST api/stop` `{force: false}`). Asking again has no further effect. */
  stop(): Promise<SessionSummary | null>;
  /** Stops without waiting (`POST api/stop` `{force: true}`). */
  forceStop(): Promise<SessionSummary | null>;
}

/**
 * Run control: start (`POST api/run`), stop in order or without waiting (`POST api/stop`), and the state for them.
 *
 * - The answer to an action (the current summary) is applied to the session copy without waiting for the notification
 *   (no stale display right after a click).
 * - Start and stop are sent separately. A stop can be asked for while the answer to a start is pending (the server
 *   delivers it once the launcher has started; stopping in order has no further effect when asked again, so if the
 *   summary in the start's answer does not show the stop as asked for yet, it is asked for again).
 * - error holds the reason for a refusal (the server's English text). It is cleared once a run starts or ends.
 * - The Promises of start() and stop() resolve with the summary in the answer (null if unreadable) and reject with an
 *   HttpError when refused. No answer (`timedOut`) is not a refusal: the session copy tells whether the run started.
 *   The state (starting and so on) has already changed when the Promise resolves.
 */
export class RunControl implements ClientRun {
  private readonly startSending: Writable<boolean>;
  private readonly stopSending: Writable<StopAction | null>;
  private readonly stopAskedWhileStarting: Writable<boolean>;
  private readonly failure: Writable<ActionFailure | null>;

  readonly starting: Readable<boolean>;
  readonly stopping: Readable<StopAction | null>;
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
    this.stopAskedWhileStarting = new Writable<boolean>(false, Object.is, onError);
    this.failure = new Writable<ActionFailure | null>(null, Object.is, onError);
    this.starting = this.startSending.readonly();
    this.stopping = this.stopSending.readonly();
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
    this.startSending.set(true);
    this.stopAskedWhileStarting.set(false);
    return this.send('api/run', request, this.startTimeoutMs() || undefined, () => {
      this.startSending.set(false);
      const asked = this.stopAskedWhileStarting.get();
      this.stopAskedWhileStarting.set(false);
      // If the summary in the start's answer does not yet reflect a stop asked for meanwhile, ask again (no further
      // effect)
      const session = this.feed.session.get();
      if (asked && session?.launcher.running && session.stopRequests === 0) {
        this.stop().catch(() => undefined);
      }
    });
  }

  /** Stops in order (waits for the participants to end). Asking again never turns it into a stop without waiting. */
  stop(): Promise<SessionSummary | null> {
    if (this.startSending.get()) this.stopAskedWhileStarting.set(true);
    this.stopSending.set('stop');
    return this.send('api/stop', { force: false }, undefined, () => this.stopSending.set(null));
  }

  /** Stops without waiting (kills the participants). */
  forceStop(): Promise<SessionSummary | null> {
    this.stopSending.set('force');
    return this.send('api/stop', { force: true }, undefined, () => this.stopSending.set(null));
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
  ): Promise<SessionSummary | null> {
    this.failure.set(null);
    return new Promise((resolve, reject) => {
      this.transport({ method: 'POST', path, body, timeoutMs }, (outcome) => {
        if (!outcome.ok) {
          const session = this.feed.session.get();
          this.failure.set({
            message: answerMissing(path, outcome.error) ?? errorMessage(outcome.error),
            session: session?.id ?? null,
            running: session?.launcher.running ?? false,
          });
          done();
          reject(outcome.error);
          return;
        }
        let summary: SessionSummary | null = null;
        try {
          summary = readSessionResponse(outcome.body);
          this.feed.adopt(summary);
        } catch (error) {
          // An answer whose summary core is unreadable is reported, and the summary of the next notification takes over
          this.feed.report(
            error instanceof ProtocolError ? `unreadable answer: ${error.message}` : String(error),
          );
        }
        done();
        resolve(summary);
      });
    });
  }
}
