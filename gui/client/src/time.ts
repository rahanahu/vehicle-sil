import { HttpTransport, errorMessage } from './http';
import { AppInfo, TimeCommand, TimeCommandName } from './protocol';
import { CommandView, SessionView, TimeView } from './session-state';
import { ErrorSink, Readable, Writable, derived, rethrowLater, shallowEqual } from './values';
import { VariablesState } from './variables';

/** The launcher features that concern time. */
export const FEATURE_SPEED = 'speed';
export const FEATURE_START_PAUSED = 'start-paused';
export const FEATURE_TIME_COMMANDS = 'time-commands';

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
 * Time state and the commands that may be used now. All values come from the session copy and the server information.
 */
export interface TimeState {
  /** Whether the launcher has time commands (pause, resume, step, speed). If not, no controls are shown. */
  supported: boolean;
  /** Time of the current session (null until a time event arrives). */
  time: TimeView | null;
  /** The speed is paced (a numeric speed). Lateness is not counted when running as fast as possible. */
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
   * Whether time commands can be sent: the launcher supports them, the session copy is in sync, the phase is running,
   * a time event has arrived, there is no result yet and no stop has been requested. Once the phase moves on to
   * stopping, the launcher no longer accepts them. Commands can also be sent during beforeStart.
   */
  controllable: boolean;
  /**
   * Whether the run is before its first step (not started yet), waiting for a resume or a step: a run started paused
   * (start_paused) whose launcher has a Control Server (the variable status is not off) and that has not started
   * moving. Meanwhile the launcher writes no time events but accepts resume and step (it accepts pause too, which
   * changes nothing), and variables written are applied in the step at time 0.
   */
  beforeStart: boolean;
  /**
   * A command sent whose answer has not been seen yet (waiting for the HTTP answer or the launcher's command event).
   * Meanwhile no further command is sent (so a double click or a repeated Enter does not step twice).
   */
  sending: TimeCommandName | null;
  /** A step is in progress (meanwhile the launcher counts as paused and accepts resume and step). */
  stepping: boolean;
  /** Whether Pause can be used (while running, also during a step). */
  canPause: boolean;
  /** Whether Resume can be used (while paused, or during a step). */
  canResume: boolean;
  /** Whether Step can be used (the same as canResume; the launcher's rule). */
  canStep: boolean;
  /** The last command that went wrong. null once the run changes. */
  failure: TimeFailure | null;
}

/** A command sent whose launcher answer (command event) has not been seen yet. */
interface Pending {
  command: TimeCommandName;
  session: number;
  /** seq of the last command event when it was sent. The next command event after it is the answer. */
  afterSeq: number;
}

/** Read side and commands of time (GuiClient.time). */
export interface ClientTime {
  /** Time state and the commands that may be used now. */
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
   * Sends a command. The result arrives as the launcher's command and time events. With no run, or while waiting for
   * the answer to the previous command, nothing is sent and `{sent: false, reason}` is returned. Check whether a
   * command can be used (canPause in state and so on) before sending.
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
 *   command, time shows that it took effect.
 */
export class TimeControl implements ClientTime {
  private readonly inFlight: Writable<number>;
  private readonly pending: Writable<Pending | null>;
  private readonly failureValue: Writable<TimeFailure | null>;
  private answerTimer: ReturnType<typeof setTimeout> | undefined;
  private lastLateness: Lateness | null = null;

  /** Time state and the commands that may be used now. */
  readonly state: Readable<TimeState>;

  constructor(
    private readonly session: Readable<SessionView | null>,
    ready: Readable<boolean>,
    app: Readable<AppInfo | null>,
    private readonly transport: HttpTransport,
    /** How long to wait for an answer (ms). */
    readonly answerTimeoutMs = ANSWER_TIMEOUT_MS,
    onError: ErrorSink = rethrowLater,
    /** Variable state (used to decide beforeStart; if absent, always false). */
    private readonly variables: Readable<VariablesState> | null = null,
  ) {
    this.inFlight = new Writable<number>(0, Object.is, onError);
    this.pending = new Writable<Pending | null>(null, Object.is, onError);
    this.failureValue = new Writable<TimeFailure | null>(null, Object.is, onError);
    this.state = derived(
      [
        session,
        ready,
        app,
        this.inFlight,
        this.pending,
        this.failureValue,
        ...(variables ? [variables] : []),
      ],
      () => this.compute(session.get(), ready.get(), app.get()),
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
   * Sends a command. The result arrives as the launcher's command and time events (the content of the HTTP answer is
   * not used). With no run, or while waiting for the answer to the previous command (sending), nothing is sent and
   * `{sent: false, reason}` is returned. If no answer comes within answerTimeoutMs, stops waiting and reports it in
   * failure. Check whether a command can be used (canPause and so on) before sending.
   */
  send(command: TimeCommand): SendResult {
    const session = this.session.get();
    if (!session) return { sent: false, reason: 'no-session' };
    if (this.state.get().sending !== null) return { sent: false, reason: 'waiting' };
    this.failureValue.set(null);
    const pending: Pending = {
      command: command.command,
      session: session.id,
      afterSeq: session.lastCommand?.seq ?? -1,
    };
    this.pending.set(pending);
    this.inFlight.update((n) => n + 1);
    clearTimeout(this.answerTimer);
    this.answerTimer = setTimeout(() => this.giveUp(pending), this.answerTimeoutMs);
    this.transport({ method: 'POST', path: 'api/time', body: command }, (outcome) => {
      this.inFlight.update((n) => n - 1);
      if (outcome.ok || this.pending.get() !== pending) return;
      clearTimeout(this.answerTimer);
      this.pending.set(null); // Not sent: no launcher answer will come
      this.failureValue.set({
        kind: 'server',
        message: errorMessage(outcome.error),
        command: pending.command,
        session: this.session.get()?.id ?? null,
      });
    });
    return { sent: true };
  }

  /** Stops the wait timer and stops waiting for the answer (when the client closes). */
  dispose(): void {
    clearTimeout(this.answerTimer);
    this.pending.set(null);
  }

  /** The answer timed out: if no answer has come yet, stop waiting and say so. */
  private giveUp(pending: Pending): void {
    if (this.pending.get() !== pending || this.state.get().sending === null) return;
    this.pending.set(null);
    this.failureValue.set({
      kind: 'no-answer',
      message:
        `the launcher did not answer the command (${pending.command}) within ` +
        `${this.answerTimeoutMs / 1000} s; it may not read its input. You can send it again.`,
      command: pending.command,
      session: pending.session,
    });
  }

  private compute(session: SessionView | null, ready: boolean, app: AppInfo | null): TimeState {
    const supported = app?.launcher.features.includes(FEATURE_TIME_COMMANDS) ?? false;
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
    const variables = this.variables?.get();
    const beforeStart =
      !!session &&
      session.startPaused &&
      session.phase === 'waiting' &&
      session.time === null &&
      session.launcher.running &&
      session.result === null &&
      variables?.session === session.id &&
      variables.status !== 'off' &&
      variables.status !== 'unknown';
    const controllable =
      supported &&
      ready &&
      !!session &&
      ((session.phase === 'running' && session.time !== null) || beforeStart) &&
      session.launcher.running &&
      session.result === null &&
      session.stopRequests === 0;
    const pending = this.pending.get();
    let sending: TimeCommandName | null = null;
    if (pending) {
      const answered =
        !session ||
        session.id !== pending.session ||
        !session.launcher.running ||
        (session.lastCommand?.seq ?? -1) > pending.afterSeq;
      sending = this.inFlight.get() > 0 || !answered ? pending.command : null;
    }
    const paused = time?.paused ?? (beforeStart ? true : null);
    const stepping = lastCommand?.name === 'step' && lastCommand.accepted && paused === false;
    const free = controllable && sending === null;
    const canResume = free && (paused === true || stepping);
    const failure = this.failureValue.get();
    return {
      supported,
      time,
      paced: typeof time?.speed === 'number',
      lateness,
      behind: (lateness?.lateSteps ?? 0) > 0,
      lastCommand,
      controllable,
      beforeStart,
      sending,
      stepping,
      canPause: free && paused === false,
      canResume,
      canStep: canResume,
      failure: failure && failure.session === (session?.id ?? null) ? failure : null,
    };
  }
}
