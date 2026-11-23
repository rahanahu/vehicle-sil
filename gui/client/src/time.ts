import { HttpTransport, errorMessage } from './http';
import { TimeCommand, TimeCommandName } from './protocol';
import { sameRunTarget, type RunTarget } from './session';
import { CommandView, SessionView, TimeView, isRunActive } from './session-state';
import { ErrorSink, Readable, Writable, derived, rethrowLater, shallowEqual } from './values';

/**
 * How long to wait for the launcher's answer (the command event) to a command sent (ms). After that, the client stops
 * waiting, enables the controls again and reports the missing answer as an error (a launcher that does not read its
 * input).
 */
export const ANSWER_TIMEOUT_MS = 5000;

/** Why nothing was sent: no run, or waiting for the answer to the previous command. */
export type NotSentReason = 'no-session' | 'waiting';

/** Result of send() and the like: whether it was sent and, if not, why. */
export type SendResult = { sent: true } | { sent: false; reason: NotSentReason };

/** A time command that went wrong. */
export interface TimeFailure {
  /** server: the server refused (message is the server's English text); no-answer: the launcher did not answer. */
  kind: 'server' | 'no-answer';
  /**
   * English text to show a person. To put it in the screen's language, build it from kind, command and answerTimeoutMs.
   */
  message: string;
  command: TimeCommandName;
  /** Number of the run it was sent to. */
  session: number | null;
}

/** Lateness figures (meaningful only while the speed is paced, such as at real-time speed). */
export interface Lateness {
  lateSteps: number;
  maxLateMs: number;
  slippedMs: number;
  lateMs: number;
}

/**
 * Time state: the virtual time, the lateness, and the answer to the last command. All values come from the session
 * copy. Whether a command may be used now is not here: the launcher says it (`allowed` of
 * its state, SessionView.launcherState), and `controls` of connect() puts it together with the connection.
 */
export interface TimeState {
  /** Time of the current session (null until a time event arrives). */
  time: TimeView | null;
  /** The speed is paced (the launcher's state has a numeric speed). Lateness is not counted when running as fast as possible. */
  paced: boolean;
  /** Lateness figures (null when the launcher does not write them). */
  lateness: Lateness | null;
  /**
   * Whether the run fell behind real time (some time steps were later than the tolerance). Small late_ms within the
   * tolerance come and go with every step synchronization, so they are not counted here. Lateness so far is kept after
   * switching to as fast as possible.
   */
  behind: boolean;
  /** The last command event received (stop included). */
  lastCommand: CommandView | null;
  /**
   * A command sent whose answer has not been seen yet (waiting for the HTTP answer or the launcher's command event).
   * Meanwhile no further command is sent (so a double click or a repeated Enter does not step twice).
   */
  sending: TimeCommandName | null;
  /** The last command that went wrong. null once the run changes. */
  failure: TimeFailure | null;
}

/** A command sent whose launcher answer (command event) has not been seen yet. */
interface Pending {
  command: TimeCommandName;
  target: RunTarget;
  /** seq of the last command event when it was sent. The next command event after it is the answer. */
  afterSeq: number;
}

/** Read side and commands of time (GuiClient.time). */
export interface ClientTime {
  /** Time state. */
  readonly state: Readable<TimeState>;
  /** How long to wait for the answer to a command (ms). */
  readonly answerTimeoutMs: number;
  pause(): SendResult;
  resume(): SendResult;
  /**
   * While paused, advances virtual time by ms and pauses again (the launcher rounds up to a multiple of the time step).
   */
  step(ms: number): SendResult;
  /** Changes the speed (a pause stays in effect). */
  setSpeed(value: number | 'max'): SendResult;
  /**
   * Sends a command. The result arrives as the launcher's command, state and time events. With no run, or while waiting for
   * the answer to the previous command, nothing is sent and `{sent: false, reason}` is returned. Whether the launcher
   * would accept the command is `allowed` of its state (or `controls` of connect()); a command it refuses comes back in
   * lastCommand with the reason.
   */
  send(command: TimeCommand): SendResult;
}

/**
 * Simulation time: exposes the state (TimeState) as a readable value and sends time commands (pause, resume, step,
 * speed change) to the GUI server (`POST api/time`).
 *
 * - The state is built from the session copy (summary and launcher events; the summary is authoritative). A client
 *   that connects midway gets the same state from the summary.
 * - The server passes commands to the launcher and returns at once. lastCommand shows whether the launcher accepted a
 *   command; the launcher's state (a pause, a resume, the speed) and time (where it is) show that it took effect.
 */
export class TimeControl implements ClientTime {
  private readonly awaitingHttp: Writable<Pending | null>;
  private readonly pending: Writable<Pending | null>;
  private readonly failureValue: Writable<{ target: RunTarget; report: TimeFailure } | null>;
  private answerTimer: ReturnType<typeof setTimeout> | undefined;
  private lastLateness: Lateness | null = null;

  /** Time state. */
  readonly state: Readable<TimeState>;

  constructor(
    private readonly session: Readable<SessionView | null>,
    private readonly transport: HttpTransport,
    private readonly currentTarget: () => RunTarget | null,
    /** How long to wait for an answer (ms). */
    readonly answerTimeoutMs = ANSWER_TIMEOUT_MS,
    onError: ErrorSink = rethrowLater,
  ) {
    this.awaitingHttp = new Writable<Pending | null>(null, Object.is, onError);
    this.pending = new Writable<Pending | null>(null, Object.is, onError);
    this.failureValue = new Writable<{ target: RunTarget; report: TimeFailure } | null>(
      null,
      Object.is,
      onError,
    );
    this.state = derived(
      [session, this.awaitingHttp, this.pending, this.failureValue],
      () => this.compute(session.get()),
      shallowEqual,
      onError,
    );
  }

  pause(): SendResult {
    return this.send({ command: 'pause' });
  }

  resume(): SendResult {
    return this.send({ command: 'resume' });
  }

  /**
   * While paused, advances virtual time by ms and pauses again (the launcher rounds up to a multiple of the time step).
   */
  step(ms: number): SendResult {
    return this.send({ command: 'step', ms });
  }

  /** Changes the speed (a pause stays in effect). */
  setSpeed(value: number | 'max'): SendResult {
    return this.send({ command: 'speed', value });
  }

  /**
   * Sends a command. The result arrives as the launcher's command, state and time events (the content of the HTTP answer is
   * not used). With no run, or while waiting for the answer to the previous command (sending), nothing is sent and
   * `{sent: false, reason}` is returned. If no answer comes within answerTimeoutMs, stops waiting and reports it in
   * failure. Whether the launcher would accept the command is `allowed` of its state.
   */
  send(command: TimeCommand): SendResult {
    const session = this.session.get();
    const target = this.currentTarget();
    if (!session || target?.session !== session.id) return { sent: false, reason: 'no-session' };
    if (this.state.get().sending !== null) return { sent: false, reason: 'waiting' };
    this.failureValue.set(null);
    const pending: Pending = {
      command: command.command,
      target,
      afterSeq: session.lastCommand?.seq ?? -1,
    };
    this.awaitingHttp.set(pending);
    this.pending.set(pending);
    clearTimeout(this.answerTimer);
    this.answerTimer = setTimeout(() => this.giveUp(pending), this.answerTimeoutMs);
    this.transport(
      { method: 'POST', path: 'api/time', body: { ...command, ...target } },
      (outcome) => {
        if (this.awaitingHttp.get() === pending) this.awaitingHttp.set(null);
        if (outcome.ok || this.pending.get() !== pending) return;
        clearTimeout(this.answerTimer);
        this.pending.set(null); // Not sent: no launcher answer will come
        if (!sameRunTarget(this.currentTarget(), target)) return;
        this.failureValue.set({
          target,
          report: {
            kind: 'server',
            message: errorMessage(outcome.error),
            command: pending.command,
            session: target.session,
          },
        });
      },
    );
    return { sent: true };
  }

  /** Stops the wait timer and stops waiting for the answer (when the client closes). */
  dispose(): void {
    clearTimeout(this.answerTimer);
    this.pending.set(null);
    this.awaitingHttp.set(null);
  }

  /** The answer timed out: if no answer has come yet, stop waiting and say so. */
  private giveUp(pending: Pending): void {
    if (this.pending.get() !== pending || this.state.get().sending === null) return;
    this.pending.set(null);
    this.failureValue.set({
      target: pending.target,
      report: {
        kind: 'no-answer',
        message:
          `the launcher did not answer the command (${pending.command}) within ` +
          `${this.answerTimeoutMs / 1000} s; it may not read its input. You can send it again.`,
        command: pending.command,
        session: pending.target.session,
      },
    });
  }

  private compute(session: SessionView | null): TimeState {
    const time = session?.time ?? null;
    let lateness: Lateness | null =
      !time || time.lateSteps === null || time.maxLateMs === null
        ? null
        : {
            lateSteps: time.lateSteps,
            maxLateMs: time.maxLateMs,
            slippedMs: time.slippedMs ?? 0,
            lateMs: time.lateMs ?? 0,
          };
    // Keep the previous object if the figures are the same (so a change in other fields alone does not report changed
    // lateness)
    if (lateness && this.lastLateness && shallowEqual(lateness, this.lastLateness)) {
      lateness = this.lastLateness;
    }
    this.lastLateness = lateness;
    const lastCommand = session?.lastCommand ?? null;
    const pending = this.pending.get();
    const target = this.currentTarget();
    let sending: TimeCommandName | null = null;
    if (pending && sameRunTarget(pending.target, target)) {
      const answered =
        !session || !isRunActive(session) || (session.lastCommand?.seq ?? -1) > pending.afterSeq;
      sending = this.awaitingHttp.get() === pending || !answered ? pending.command : null;
    }
    const failure = this.failureValue.get();
    return {
      time,
      paced: typeof session?.launcherState?.speed === 'number',
      lateness,
      behind: (lateness?.lateSteps ?? 0) > 0,
      lastCommand,
      sending,
      failure: failure && sameRunTarget(failure.target, target) ? failure.report : null,
    };
  }
}
