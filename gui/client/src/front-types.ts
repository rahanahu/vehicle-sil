// Shapes of the front door (`connect()` in front.ts). Data fields use the wire's names (snake_case: the same words as
// the launcher's `state` event and the GUI server's `allowed`); functions use JavaScript's (`forceStop`, `runUntil`).
import type { GuiClient } from './client';
import type { Finding, ScenarioStatus, ScenarioStep } from './protocol';
import type { ValueCondition, WriteBlock, WriteStatus } from './variable-state';
import type { VariableRef, VariableValue } from './variables';
import type { Readable } from './values';

/** A reason: a code that stays the same (an entry of the launcher's reason codes, or one the client names below) and one English sentence. */
export interface Reason {
  code: string;
  message: string;
}

/** How the page stands with the GUI server. */
export type Link = 'connecting' | 'online' | 'offline' | 'rejected' | 'incompatible';

/**
 * Where the run stands. `starting`: a start is on its way (this page's or another page's) or the launcher has not
 * begun; `waiting`: the launcher waits for the participants to join (phase `waiting`; a run held before the first step with start_paused is `paused`); `ended`: the
 * run has a result; `crashed`: the launcher ended without one.
 */
export type RunState =
  'none' | 'starting' | 'waiting' | 'running' | 'paused' | 'stopping' | 'ended' | 'crashed';

/** How a run ended. `crashed` is the client's own: the launcher ended without writing a result. */
export type EndKind =
  'completed' | 'stopped' | 'interrupted' | 'failed' | 'setup_failed' | 'crashed';

/** The result of the last run. */
export interface FrontResult {
  end: EndKind;
  /** The scenario's verdict (null for a run without a scenario). */
  scenario: ScenarioStatus | null;
  /** Whether the run succeeded. A stop by the operator is not a failure. */
  ok: boolean;
  /** One English sentence for a person. */
  message: string;
}

/** What a screen shows as the state (discrete values only; the time is in `clock`). */
export interface FrontStatus {
  link: Link;
  run: RunState;
  /** The configuration file of the run (the last one if none is running); the start-up file when there has been no run. */
  file: string | null;
  /** Set while `run` is `ended` or `crashed`. */
  result: FrontResult | null;
  /** The one problem to tell the user now (null if none). The connection comes first; then the newer of the launcher's crash, the last action's failure and a failed run. */
  problem: Reason | null;
}

/** Whether the run keeps to its speed. */
export type Pacing = 'not-paced' | 'on-time' | 'late';

/** Time and speed (changes often, so it is apart from `status`). */
export interface FrontClock {
  /** Virtual time (ms); null until the first time event. */
  sim_ms: number | null;
  /** The speed that was asked for. */
  speed: number | 'max' | null;
  /** The speed over about the last second; null while paused or just started. */
  actual_speed: number | null;
  pacing: Pacing;
}

/** Whether one operation can be used now. `ok` is for a button: it is false also when the operation would change nothing. */
export interface Control {
  ok: boolean;
  /** Why not (null if ok). */
  reason: Reason | null;
  /** The operation is accepted, but something is worth knowing first (null if none). */
  caution: Reason | null;
}

/** What each operation can do now. Built from the launcher's `state` and the GUI server's `allowed`. */
export interface FrontControls {
  start: Control;
  stop: Control;
  force_stop: Control;
  pause: Control;
  resume: Control;
  step: Control & { min_ms: number; max_ms: number };
  run_until: Control;
  speed: Control & { min: number; max: number };
}

/** The operations that have a control. */
export type OperationName = keyof FrontControls;

/** The progress of the scenario of the run. */
export interface FrontScenario {
  file: string;
  name: string | null;
  status: ScenarioStatus | null;
  steps: readonly ScenarioStep[];
  /** Where the server serves the timeline of the run (null until the run has ended with one). */
  timeline_url: string | null;
}

/**
 * An action that did not go through. `refused`: the owner said no (nothing happened). `unknown`: it may have been
 * done (no answer, an answer that cannot be tied to the command, interrupted before it arrived): look at `status` and
 * `clock` before sending it again.
 */
export class ActionError extends Error {
  constructor(
    /** A code that stays the same: the owner's `reason` (`not-paused`, `check-failed`, ...) or the client's (`busy`, `no-answer`, `interrupted`, `timeout`, ...). */
    readonly code: string,
    message: string,
    readonly outcome: 'refused' | 'unknown',
    /** For `check-failed`: the findings of the check that refused the start. */
    readonly findings: readonly Finding[] = [],
  ) {
    super(message);
    this.name = 'ActionError';
  }
}

/** What a start asks for (the part of the intent that the GUI server takes now). */
export interface Intent {
  disable?: string[];
  manual?: string[];
  scenario?: string | null;
  variant?: string | null;
  params?: Record<string, string | number>;
  speed?: number | 'max';
  start_paused?: boolean;
}

/** Operations of the run. Each resolves when the server has taken the request; whether the run began is `status.run`. */
export interface FrontRun {
  /** Starts a run of `file` (default: the start-up file). Rejects with `check-failed` and the findings if the check refuses it. */
  start(intent?: Intent, options?: { file?: string }): Promise<void>;
  /** Stops in order. Works whatever the state of the connection (the request is sent if the network is up). */
  stop(): Promise<void>;
  /** Stops without waiting for the participants. */
  forceStop(): Promise<void>;
}

/**
 * Time operations. `pause`, `resume` and `setSpeed` resolve when the launcher has answered; `step` and `runUntil`
 * resolve when time has arrived and the run is paused again (read `clock` after `await`). Another command sent before
 * it arrives (from this page, another page or stdin) rejects it with `interrupted`.
 */
export interface FrontTime {
  pause(): Promise<void>;
  resume(): Promise<void>;
  step(ms: number): Promise<void>;
  setSpeed(value: number | 'max'): Promise<void>;
  runUntil(ms: number): Promise<void>;
}

/** What a screen shows for one variable. */
export interface VariableView {
  value: VariableValue;
  /** The value as text (an enum shows its item's name). Empty while there is no value. */
  text: string;
  unit: string;
  min: number | null;
  max: number | null;
  step: number | null;
  /** Virtual time of the step that read the value (ms). */
  sim_ms: number | null;
  /** The value may be out of date. */
  stale: boolean;
  /** One English sentence when the value is not a live one (waiting, paused, stale, ended, ...); null while live. */
  note: string | null;
  /** Whether it can be written: the variable's own conditions and what the launcher accepts (`state.allowed.write`). */
  can_write: boolean;
  /** Why the variable cannot be written (null if it can). */
  why: Reason | null;
  /** A write is on its way. */
  writing: boolean;
  /** Why the last write of this page did not go through (null if none). */
  problem: Reason | null;
  condition: ValueCondition;
  block: WriteBlock | null;
  write_status: WriteStatus;
}

/**
 * A variable. Values arrive while someone subscribes (the first subscription asks the server for them, the last
 * unsubscribe stops it: there is no close()). `write` needs no subscription.
 */
export interface FrontVariable extends Readable<VariableView> {
  /** Resolves when the value has been applied in an ECU step. Rejects with an ActionError (`no-answer` is `unknown`: it is not sent again). */
  write(value: number | boolean | string): Promise<void>;
}

/** The front door: `connect()` returns it. */
export interface Front {
  /** The client of the same connection (events, logs, findings, participants, the write trail). */
  readonly low: GuiClient;
  readonly status: Readable<FrontStatus>;
  readonly clock: Readable<FrontClock>;
  readonly controls: Readable<FrontControls>;
  readonly scenario: Readable<FrontScenario | null>;
  readonly run: FrontRun;
  readonly time: FrontTime;
  /** A variable by `"Peer.variable"` (cut at the first dot) or `{peer, name}` (for a peer whose name has a dot). */
  variable(ref: string | VariableRef): FrontVariable;
  /** Disconnects and fails what is waiting. A connection that `connect()` shares is made again by the next `connect()`. */
  close(): void;
}
