// Shapes of the front door (`connect()` in front.ts). Data fields use the wire's names (snake_case: the same words as
// the launcher's `state` event and the GUI server's `allowed`); functions use JavaScript's (`forceStop`, `runUntil`).
import type { GuiClient } from './client';
import type { RunTarget } from './session';
import type {
  AfterScenario,
  CheckResult,
  CheckScenarioParam,
  ConfigList,
  ConfigText,
  SavedConfig,
  Finding,
  Open,
  OutsideState,
  Recording,
  RunAck,
  RunEnd,
  RuntimeStatus,
  ScenarioFile,
  ScenarioStatus,
  ScenarioStep,
} from './protocol';
import type { ValueCondition, WriteBlock, WriteStatus } from './variable-state';
import type { VariableRef, VariableValue, WriteWaitOptions } from './variables';
import type { Readable } from './values';

import type { Reason } from './session-state';

/** A reason: a code that stays the same and one English sentence (defined with the session copy, which reads them too). */
export type { Reason };

/** How the page stands with the GUI server. */
export type Link = 'connecting' | 'online' | 'offline' | 'rejected' | 'incompatible';

/**
 * Where the run stands. `starting`: a start is on its way (this page's or another page's) or the launcher has not
 * begun; `waiting`: the launcher waits for the participants to join (phase `waiting`; a run held before the first step with start_paused is `paused`); `ended`: the
 * run has a result; `crashed`: the launcher ended without one.
 */
export type RunState =
  'none' | 'starting' | 'waiting' | 'running' | 'paused' | 'stopping' | 'ended' | 'crashed';

/**
 * How a run ended, as the launcher's result says (`end`). `crashed` is the client's own: the launcher ended without
 * writing a result. A launcher newer than this library may write a value that is not listed.
 */
export type EndKind = RunEnd | 'crashed';

/** The result of the last run. */
export interface FrontResult {
  /** null when nothing was started: the check failed, or the file or the command line could not be used. */
  end: EndKind | null;
  /** The scenario's verdict (null for a run without a scenario). */
  scenario: ScenarioStatus | null;
  /**
   * Whether the run succeeded, as the launcher's result says (`ok`: its exit code is 0). A stop by the operator is a
   * success, unless it cut a scenario short (the scenario has no verdict then).
   */
  ok: boolean;
  /** One English sentence for a person, as the launcher wrote it. */
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
  /** `fastest`: whether the speed `'max'` is accepted besides the numbers from `min` to `max`. */
  speed: Control & { min: number; max: number; fastest: boolean };
  /**
   * `saveConfig()`, from the GUI server's `allowed.save` (`save-disabled`: started with `--no-save`). `caution`
   * `command-changes-allowed`: the server also saves edits that change the commands that run, and new files; show it
   * near the save button. Whether one edit is saved is the server's answer to it, not this.
   */
  save: Control;
}

/** The operations that have a control. */
export type OperationName = keyof FrontControls;

/** The progress of the scenario of the run. */
export interface FrontScenario {
  file: string;
  name: string | null;
  /** Selected case name, if any. */
  case?: string | null;
  /** Resolved parameters of the scenario, if any. */
  params?: readonly CheckScenarioParam[];
  status: ScenarioStatus | null;
  handed_over: boolean;
  over_at_ms: number | null;
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

/**
 * What a start asks for: the keys of the request file (`schema/run-request.schema.json`), with its names and value
 * forms. A choice that is not made is left out of the object: there is no null.
 */
export interface Intent {
  /** The format of the request file: 1, or left out (the client adds none). */
  vehicle_sil_run_request?: 1;
  disable?: string[];
  enable?: string[];
  manual?: string[];
  /**
   * A `path` of `scenarios()` (relative to the root of the GUI server, as the list gives it; the server turns it into
   * the path the request file needs, which is relative to the configuration file's directory). Do not compose it.
   */
  scenario?: string;
  /** The named case of the scenario (`cases:`). Requires `scenario`. */
  case?: string;
  /** What to do once the scenario passes (`stop`, or `pause` to hand over to manual control). */
  after_scenario?: AfterScenario;
  /** Extra signals (`<participant>.<variable>`) to record on the timeline independent of scenario assertions. */
  record?: string[];
  /** Virtual milliseconds to keep recording sampled signals after a scenario assertion/timeout failure (1..600000). */
  record_after_fail_ms?: number;
  /** true: record virtual-timestamped structured ECU events (`Ecu::log` / `Agent::log`) on the timeline. */
  record_events?: boolean;
  variant?: string;
  params?: Record<string, string | number | boolean>;
  speed?: number | 'max';
  start_paused?: boolean;
  /** Exact virtual time at which to stop. Omit for no automatic stop. */
  stop_at_ms?: number;
  /** Wall-clock milliseconds of lateness before a paced step counts as late. */
  late_tolerance_ms?: number;
  strict?: boolean;
  /**
   * Network name to where it is connected outside the simulation: a `value` that `connections()` gave with `usable`
   * true, as written there, or `'none'` (SIL Kit only; the same as leaving the network out). The server refuses a value
   * that its last list of that network did not give (`target-not-listed`).
   */
  connect?: Record<string, string>;
  /** A run connected outside keeps real time (`realtime`, the launcher's default) unless `free` is chosen. */
  time_policy?: Open<'realtime' | 'free'>;
}

/** A place where a network can be connected, as the bridge of one backend listed it. */
export interface ConnectTarget {
  /** The bridge's backend (`packet`, `tap`, `socketcan`, `npcap`, ...); null if the list did not say. */
  backend: string | null;
  /** What `connect` gives the network. */
  value: string;
  label: string;
  /** One English line from the bridge (state, and a device in use by another bridge). */
  detail: string;
  /** false: it cannot be chosen; `reason` says why. */
  usable: boolean;
  reason: string | null;
}

/** A backend whose bridge could not list (it is missing on this computer, or it failed). */
export interface ConnectListError {
  backend: string | null;
  /** The launcher's English sentence. */
  error: string;
}

/** One entry of `connections()`: a target, or a backend that has no list. */
export type ConnectEntry = ConnectTarget | ConnectListError;

/**
 * A network connected outside the simulation in the run (`outside[]` of the launcher's `state`), with the counts of its
 * `outside` events.
 */
export interface Outside {
  network: string;
  target: string | null;
  /** The bridge (`bridge.<network>`). */
  participant: string | null;
  /** Ethernet: `up`, `down` or `unknown` (null before the bridge has said). */
  link: OutsideState['link'];
  /** CAN: `active`, `passive`, `bus-off` or `unknown` (null for Ethernet, or before the bridge has said). */
  bus: OutsideState['bus'];
  /** `ok`, `interrupted` or `degraded`. */
  condition: OutsideState['condition'];
  /** Codes, in the launcher's order (empty when the condition is ok). */
  reasons: readonly string[];
  /** Frames went to the bus and nothing came back for 10 s (apart from `condition`, which can still be `ok`). */
  nothing_received: boolean;
  /** Frames the bridge sent to the bus so far; null until an `outside` event has arrived on this page. */
  to_outside: number | null;
  /** Frames that came from a device on the bus so far; null until an `outside` event has arrived on this page. */
  from_outside: number | null;
}

/** Operations of the run. Each resolves when the server has taken the request; whether the run began is `status.run`. */
export interface FrontRun {
  /** Starts a run of `file` (default: the start-up file). Rejects with `check-failed` and the findings if the check refuses it. */
  start(
    intent?: Intent,
    options?: { file?: string; onStarted?: (started: RunAck) => void },
  ): Promise<void>;
  /** Stops in order. Works whatever the state of the connection (the request is sent if the network is up). */
  stop(): Promise<void>;
  /** Stops without waiting for the participants. */
  forceStop(target?: RunTarget): Promise<void>;
}

/**
 * Time operations. `pause`, `resume` and `setSpeed` resolve when the launcher has answered; `step` and `runUntil`
 * resolve when time has arrived and the run is paused again (read `clock` after `await`). Another command sent before
 * it arrives (from this page, another page or stdin) rejects it with `interrupted`.
 * Pass the target from the rendered view to refuse the operation if the run changes before dispatch.
 */
export interface FrontTime {
  pause(target?: RunTarget | null): Promise<void>;
  resume(target?: RunTarget | null): Promise<void>;
  step(ms: number, target?: RunTarget | null): Promise<void>;
  setSpeed(value: number | 'max', target?: RunTarget | null): Promise<void>;
  runUntil(ms: number, target?: RunTarget | null): Promise<void>;
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
  /**
   * Whether the ECU uses the variable now (information only: `can_write`, `why`, `note` and `condition` do not look at
   * it). `active` from a server that does not say; `unknown` while the ECU has not said or the variable is not in the run.
   */
  availability: 'unknown' | 'active' | 'inactive';
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

/** Options of `variable.enqueue(value, options)` and `variable.write(value, options)`. */
export interface FrontWriteOptions {
  /** Virtual time at which to apply the write (ms). Omitted: the next step of the ECU. */
  atMs?: number;
  /** If `atMs` cannot be met: `'reject'` (default; not applied) or `'apply'` (applied in the next step). */
  late?: 'reject' | 'apply';
  /** Watchdog timeout budget (ms; default `WRITE_TIMEOUT_MS`). Must be a finite number greater than 0. */
  timeoutMs?: number;
  /** Forwarded to `fetch(..., { keepalive })` so a release write sent on `pagehide` can outlive the page. */
  keepalive?: boolean;
  /** Aborts the write or wait (`ActionError('aborted', ...)`). */
  signal?: AbortSignal;
}

/** Confirmed application of a variable write (`queued.applied()` / `variable.write()`). */
export interface WriteResult {
  readonly session: string;
  readonly seq: number;
  readonly peer: string;
  readonly name: string;
  readonly value: number | boolean | string;
  readonly appliedAtMs: number | null;
  readonly late: boolean;
}

/** Handle returned once the server accepted a variable write and the ECU queued it (`variable.enqueue()`). */
export interface QueuedWrite {
  readonly session: string;
  readonly seq: number;
  readonly peer: string;
  readonly name: string;
  readonly value: number | boolean | string;
  /** Resolves with `WriteResult` once the ECU applies the write in a step; rejects with `ActionError` otherwise. */
  applied(options?: WriteWaitOptions): Promise<WriteResult>;
}

/**
 * A variable. Values arrive while someone subscribes (the first subscription asks the server for them, the last
 * unsubscribe stops it: there is no close()). `enqueue` and `write` need no subscription.
 */
export interface FrontVariable extends Readable<VariableView> {
  /**
   * Subscribes to changes of this variable's view. Optionally requests a polling interval (`periodMs`, finite
   * `20..60_000` ms; default: the client's `watchPeriodMs`, or `WATCH_PERIOD_MS` = 100) while this subscription is
   * active; the shortest active interval across all subscribers wins, and the slower interval is restored when faster
   * subscribers unsubscribe. Note: latest-value polling at 20 ms does not guarantee capturing pulses shorter than 20 ms.
   */
  subscribe(
    listener: (value: VariableView) => void,
    options?: {
      /** Requested polling interval (ms; finite `20..60_000`, rounded to `WATCH_PERIODS_MS`). */
      periodMs?: number;
    },
  ): () => void;
  /**
   * Sends the write and resolves once the server accepted it and the ECU queued it (stages 1+2).
   * Rejects immediately with an `ActionError` if refused or already terminally failed (`invalid`, `busy`, `late`, ...).
   */
  enqueue(value: number | boolean | string, options?: FrontWriteOptions): Promise<QueuedWrite>;
  /**
   * Resolves with `WriteResult` when the value has been applied in an ECU step (`enqueue(v, opts).then(q => q.applied(...))`).
   * Rejects with an `ActionError` (`no-answer` is `unknown`: it is not sent again).
   */
  write(value: number | boolean | string, options?: FrontWriteOptions): Promise<WriteResult>;
}

/** The front door: `connect()` returns it. */
export interface Front {
  /** The client of the same connection (events, logs, findings, participants, the write trail). */
  readonly low: GuiClient;
  readonly status: Readable<FrontStatus>;
  readonly clock: Readable<FrontClock>;
  readonly controls: Readable<FrontControls>;
  readonly scenario: Readable<FrontScenario | null>;
  /** Current persistent runtime state (mode, ready/preparing/resetting/cold reason), or null until received. */
  readonly runtime: Readable<RuntimeStatus | null>;
  readonly run: FrontRun;
  readonly time: FrontTime;
  /**
   * Prepares the persistent launcher for `options.file` (default: the start-up file) when eligible (`POST api/prepare`).
   */
  prepare(options?: {
    file?: string;
    variant?: string;
    disable?: string[];
    enable?: string[];
    manual?: string[];
  }): Promise<RuntimeStatus | null>;
  /** Lists the configuration files under the server root (`GET api/configs`). */
  configs(): Promise<ConfigList>;
  /** Returns applied writes recorded in the current or last run as scenario YAML (`GET api/recording`). */
  recording(): Promise<Recording | null>;
  /**
   * The networks of the run that the start's `connect` connected outside (empty when there are none, when no run is
   * going, and in a run that a `role: bridge` participant of the file connects: do not read "connected or not" from it).
   * Changes as the launcher's `state` and `outside` events arrive.
   */
  readonly outside: Readable<readonly Outside[]>;
  /**
   * Where `network` of `file` (default: the start-up file, as for `run.start`) can be connected on the computer of the
   * GUI server: the entries of every backend, in the launcher's order. It starts that network's bridge to list it (on
   * Windows, Npcap may ask to raise the rights): call it when the user opens the choice, not when the page opens. A
   * backend that cannot list is an entry with `error`, not a failure. Rejects with an ActionError (`check-busy`,
   * `unknown-network`, ...).
   */
  connections(request: { network: string }, options?: { file?: string }): Promise<ConnectEntry[]>;
  /**
   * The scenario files next to `file` (default: the start-up file, as for `run.start` and `connections()`; pass the
   * same `file` to this and to `run.start` when a run uses another file), as the list that
   * `client.low.scenarios(file)` gives, without its directory. Give `path` of an entry as `intent.scenario`. Rejects
   * with an ActionError (`no-file` when there is no file to ask for, and the server's code otherwise).
   */
  scenarios(options?: { file?: string }): Promise<ScenarioFile[]>;
  /**
   * Checks what `intent` would start, without starting it (`POST api/check`), and returns the launcher's check result
   * (`findings`; `time_lock` and `networks[].outside` are in `raw`). Rejects with an ActionError (`target-not-listed`,
   * `check-busy`, `unknown-field`, ...); a failure is not put into `status.problem`. It sends one check at a time (the
   * next after the answer to the one before), and a check of the same intent and file as one that waits or is on its
   * way shares it (`fresh`: only one that waits, so that the answer reads the file as it is now). A check that waits is
   * not sent when `signal` has aborted for every caller of it (it rejects with the signal's reason), nor after
   * `close()` (`closed`).
   *
   * With `text`, it checks that text as the content of `file` (a text that is not saved yet; no process starts). The
   * intent then takes only `variant`, `strict` and `vehicle_sil_run_request` (another field is `unknown-field`). A check of a text is never
   * shared with another check, and its answer is the caller's alone: the parts of the page do not see it.
   */
  check(
    intent: Pick<Intent, 'vehicle_sil_run_request' | 'variant' | 'strict'>,
    options: { file?: string; text: string; signal?: AbortSignal },
  ): Promise<CheckResult>;
  check(
    intent?: Intent,
    options?: { file?: string; signal?: AbortSignal; fresh?: boolean },
  ): Promise<CheckResult>;
  /**
   * Reads the text of a configuration file as it is saved (`GET api/config`), with the SHA-256 of its bytes (`stamp`).
   * `file` defaults to the start-up file (`no-file` if there is none). Rejects with an ActionError
   * (`not-a-configuration`, `config-too-large`, `config-unreadable`, ...); a failure is not put into `status.problem`.
   */
  readConfig(options?: { file?: string }): Promise<ConfigText>;
  /**
   * Describes the participants of the saved `file` again (`POST api/describe`: after an executable was built again),
   * so that the checks of a text compare them, and returns that check as `check()` does. The answer is the caller's
   * alone: it does not change what the parts of the page show. A describe of the same file and variant that waits or is on its way is shared. Rejects with an ActionError (`check-busy`,
   * `check-timeout`, ...); a failure is not put into `status.problem`.
   */
  describeAgain(options?: { file?: string; variant?: string }): Promise<CheckResult>;
  /**
   * Saves `text` as the content of `file` (`POST api/config`), with the `stamp` that `readConfig()` gave (null: a new
   * file), and resolves with the stamp of the saved content. By default the server saves only an edit that keeps the
   * commands that run, as the launcher's check of the text says (`controls.save` tells what applies). `file` defaults
   * to the start-up file (`no-file` if there is none). Rejects with an ActionError: `save-disabled`,
   * `commands-changed` (`findings` are the launcher's; also a new file, `stamp` null, to a server started without
   * `--allow-command-changes`), `config-changed` (read the file again and make the edit on
   * that text), `config-busy`, `config-unreadable`, `text-too-large`, ...; a failure is not put into `status.problem`.
   */
  saveConfig(options: { file?: string; text: string; stamp: string | null }): Promise<SavedConfig>;
  /** A variable by `"Peer.variable"` (cut at the first dot) or `{peer, name}` (for a peer whose name has a dot). */
  variable(ref: string | VariableRef): FrontVariable;
  /** Disconnects and fails what is waiting. A connection that `connect()` shares is made again by the next `connect()`. */
  close(): void;
}
