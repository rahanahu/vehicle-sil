/**
 * The shapes the launcher decides: `vehicle_sil_launch check --json`, `run --json` and the events of `run --events`.
 * The GUI server passes them on unchanged (in the session summary, the `launcher` notification and api/check), so their
 * versions are the launcher's. Rules of the parsers: read.ts. Every object these parsers return keeps the launcher's
 * object in `raw` (FromLauncher), so a key that the types do not name still reaches screens and panels.
 */

import {
  Fields,
  Open,
  Reader,
  Unreadable,
  array,
  boolean,
  field,
  integer,
  items,
  lenient,
  nullable,
  number,
  object,
  optional,
  record,
  soft,
  string,
  version,
  withRaw,
} from './read';

/**
 * Speed. 'max' is as fast as possible; a number is a factor (1 is real-time speed). null means the launcher's default
 * (the value in the configuration file) when given at the start, and "not written" in a value the launcher writes.
 */
export type Speed = null | number | 'max';

export const readSpeed: Reader<Speed> = (value, path) =>
  value === null || value === 'max' ? value : number(value, path);

/**
 * A JSON object as the launcher wrote it. The library neither checks nor interprets its values: check the type of a
 * value before using it, and insert it into a page as text, never as HTML.
 */
export type LauncherFields = Readonly<Record<string, unknown>>;

/**
 * What every object read from the launcher's output has besides its typed fields. The typed fields follow the
 * launcher's format versions (`vehicle_sil_check`, `vehicle_sil_run`, `vehicle_sil_events`), not the version of this
 * library; a key that the launcher adds is in `raw` without a new version of the library.
 */
export interface FromLauncher {
  /**
   * The object the fields were read from, with every key, including those this type does not name (for the finding of
   * a finding event, the step of a scenario_step event and the link of a control_link event: the event). Set by the
   * parsers of this library, read-only and not enumerable (JSON.stringify, Object.keys, deep equality and spreading
   * `{...x}` leave it out); absent on an object built by hand.
   */
  readonly raw?: LauncherFields;
}

// --- Launcher: check (same shape as check --json) ------------------------------------------------

/** Severity of a finding. An unknown severity is treated as an error (severityRank). */
export type Severity = Open<'error' | 'warning' | 'note'>;

/** A finding of the check or the run. message and detail are shown in English, unchanged. */
export interface Finding extends FromLauncher {
  severity: Severity;
  code: string;
  file: string | null;
  line: number | null;
  participant: string | null;
  network: string | null;
  message: string;
  detail: string | null;
}

/** Rank for sorting by severity, most severe first (error 0, warning 1, note 2; an unknown severity ranks as error). */
export function severityRank(severity: Severity): number {
  return severity === 'note' ? 2 : severity === 'warning' ? 1 : 0;
}

/** Reads a finding. With u, part by part (if the severity and text cannot be read, the finding is unreadable). */
export const readFinding = (value: unknown, path: string, u?: Unreadable): Finding => {
  const f = object(value, path);
  return withRaw(f, {
    severity: field(f, path, 'severity', string),
    code: field(f, path, 'code', lenient(string, '', u)),
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    participant: field(f, path, 'participant', soft(nullable(string), null, u)),
    network: field(f, path, 'network', soft(nullable(string), null, u)),
    message: field(f, path, 'message', string),
    detail: field(f, path, 'detail', soft(nullable(string), null, u)),
  });
};

/** A variable the ECU declared about itself (so panels can be drawn before the run). */
export interface Variable extends FromLauncher {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  causality: Open<'input' | 'output' | 'parameter'>;
  unit: string;
  min: number | null;
  max: number | null;
  /**
   * true for a variable vehicle-sil adds to every ECU (`ecu.connected` and so on), false for a variable the ECU
   * declared. null from an older launcher that does not write it.
   */
  builtin: boolean | null;
}

const readVariable: Reader<Variable> = (value, path) => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', string),
    causality: field(f, path, 'causality', string),
    unit: field(f, path, 'unit', optional(string, '')),
    min: field(f, path, 'min', optional(nullable(number), null)),
    max: field(f, path, 'max', optional(nullable(number), null)),
    builtin: field(f, path, 'builtin', optional(nullable(boolean), null)),
  });
};

// --- Launcher: scenarios (docs/scenario.md) -----------------------------------------------------

/** Launcher feature: can run scenarios (`--scenario`). */
export const FEATURE_SCENARIO = 'scenario';
/**
 * Launcher feature: can keep a Control Server during a scenario run (variables can be read; the launcher refuses
 * writes). With a launcher without it, a scenario run has no variables.
 */
export const FEATURE_SCENARIO_CONTROL = 'scenario-control';

// --- Launcher: participants that the user starts ("Starting a participant yourself" in docs/simulation-config.md) ---

/**
 * Launcher feature: participants the user starts (`manual: true` in the configuration file, `--manual`). The launcher
 * does not start them; it reports the command that starts them in a participant event (state `manual`) and waits for
 * them to join.
 */
export const FEATURE_MANUAL = 'manual-participants';

/** State of a scenario step. running only appears in events (not in the steps of the result). */
export type ScenarioStepStatus = Open<'running' | 'passed' | 'failed' | 'unfinished' | 'not_run'>;

/** Overall result of a scenario. */
export type ScenarioStatus = Open<'passed' | 'failed' | 'incomplete' | 'not_run'>;

/**
 * One scenario step (the `scenario_step` event, `scenario.steps[]` of the result, `scenario.steps[]` of the summary).
 */
export interface ScenarioStep extends FromLauncher {
  /** From 1. */
  index: number;
  /** Line in the scenario file. */
  line: number | null;
  kind: Open<'set' | 'ramp' | 'wave' | 'dither' | 'expect' | 'wait_until'>;
  /** Text of the step (shown in English, unchanged). */
  text: string;
  status: ScenarioStepStatus;
  /** Time of the step (virtual time, ms). */
  start_ms: number | null;
  /** Virtual time at which the step's result was decided. */
  at_ms: number | null;
  /** Virtual time at which the step ends (null if not fixed). */
  end_ms: number | null;
  /** code and text of the failure finding (shown in English, unchanged). */
  code: string | null;
  message: string | null;
}

const readScenarioStepFields = (f: Fields, path: string, u?: Unreadable): ScenarioStep =>
  withRaw(f, {
    index: field(f, path, 'index', integer),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    kind: field(f, path, 'kind', lenient(string, '', u)),
    text: field(f, path, 'text', lenient(string, '', u)),
    status: field(f, path, 'status', string),
    start_ms: field(f, path, 'start_ms', soft(nullable(number), null, u)),
    at_ms: field(f, path, 'at_ms', soft(nullable(number), null, u)),
    end_ms: field(f, path, 'end_ms', soft(nullable(number), null, u)),
    code: field(f, path, 'code', soft(nullable(string), null, u)),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
  });

export const readScenarioStep = (value: unknown, path: string, u?: Unreadable): ScenarioStep =>
  readScenarioStepFields(object(value, path), path, u);

/** `scenario` of the check result (only with `--scenario`). */
export interface CheckScenario extends FromLauncher {
  /** The scenario file (as passed to the launcher). */
  file: string;
  name: string | null;
  /** Number of steps. */
  steps: number;
}

const readCheckScenario = (value: unknown, path: string, u?: Unreadable): CheckScenario => {
  const f = object(value, path);
  return withRaw(f, {
    file: field(f, path, 'file', lenient(string, '', u)),
    name: field(f, path, 'name', soft(nullable(string), null, u)),
    steps: field(f, path, 'steps', lenient(integer, 0, u)),
  });
};

/** `scenario` of the run result (only with `--scenario`). */
export interface ScenarioResult extends FromLauncher {
  file: string;
  name: string | null;
  status: ScenarioStatus;
  /** The controller step at which the scenario ended (passed and failed; null otherwise). */
  over_at_ms: number | null;
  steps: ScenarioStep[];
}

const readScenarioResult = (value: unknown, path: string, u?: Unreadable): ScenarioResult => {
  const f = object(value, path);
  return withRaw(f, {
    file: field(f, path, 'file', lenient(string, '', u)),
    name: field(f, path, 'name', soft(nullable(string), null, u)),
    status: field(f, path, 'status', lenient(string, '', u)),
    over_at_ms: field(f, path, 'over_at_ms', soft(nullable(number), null, u)),
    steps: field(
      f,
      path,
      'steps',
      items((v, p) => readScenarioStep(v, p, u), u),
    ),
  });
};

export type CheckStatus = Open<'described' | 'not_checked' | 'failed' | 'skipped' | 'disabled'>;

/** What a participant is for (`role` in the configuration file, "Test participants and bridges" in docs/simulation-config.md). */
export type ParticipantRole = Open<'ecu' | 'test' | 'bridge'>;

export interface CheckParticipant extends FromLauncher {
  /** null if unreadable. */
  name: string | null;
  line: number | null;
  required: boolean | null;
  enabled: boolean | null;
  /**
   * Started by the user (`manual: true` in the configuration file, or `--manual`). Older launchers do not write it, so
   * false.
   */
  manual: boolean;
  step_ms: number | null;
  status: CheckStatus;
  /** Self-declared variables. null means unknown (not declared, left out, or unreadable); an empty array means none. */
  variables: Variable[] | null;
  /** How the ECU declared it exposes its variables. null if not declared, not checked, or from an older launcher. */
  control: ControlOffer | null;
  /** What the participant is for. null from an older launcher that does not write it (absent on an object built by hand). */
  role?: ParticipantRole | null;
}

const readCheckParticipant = (value: unknown, path: string, u?: Unreadable): CheckParticipant => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', nullable(string)),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    required: field(f, path, 'required', lenient(nullable(boolean), null, u)),
    enabled: field(f, path, 'enabled', lenient(nullable(boolean), null, u)),
    manual: field(f, path, 'manual', soft(boolean, false, u)),
    step_ms: field(f, path, 'step_ms', lenient(nullable(number), null, u)),
    status: field(f, path, 'status', lenient(string, '', u)),
    variables: field(
      f,
      path,
      'variables',
      soft(nullable(u ? items(readVariable, u) : array(readVariable)), null, u),
    ),
    control: field(
      f,
      path,
      CONTROL_CHECK_KEY,
      soft(
        nullable((v, p) => readControlOffer(v, p, u)),
        null,
        u,
      ),
    ),
    role: field(f, path, 'role', soft(nullable(string), null, u)),
  });
};

export interface CheckNetwork extends FromLauncher {
  name: string | null;
  kind: string | null;
  /** Participant names in use (in configuration file order). */
  participants: string[];
}

const readCheckNetwork = (value: unknown, path: string, u?: Unreadable): CheckNetwork => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', lenient(nullable(string), null, u)),
    kind: field(f, path, 'kind', lenient(nullable(string), null, u)),
    participants: field(f, path, 'participants', items(string, u)),
  });
};

export type CheckOutcome = Open<'passed' | 'failed' | 'unreadable' | 'usage'>;

/** Result of `vehicle_sil_launch check --json` (format version 1). */
export interface CheckResult extends FromLauncher {
  file: string | null;
  result: CheckOutcome;
  errors: number;
  warnings: number;
  notes: number;
  stop_at_ms: number | null;
  participants: CheckParticipant[];
  networks: CheckNetwork[];
  findings: Finding[];
  /** The scenario (only for a check with `--scenario`; null otherwise). */
  scenario: CheckScenario | null;
}

/** Reads a check result. With u, part by part. */
export const readCheckResult = (value: unknown, path: string, u?: Unreadable): CheckResult => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_check', 1, u);
  return withRaw(f, {
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    result: field(f, path, 'result', lenient(string, '', u)),
    errors: field(f, path, 'errors', lenient(integer, 0, u)),
    warnings: field(f, path, 'warnings', lenient(integer, 0, u)),
    notes: field(f, path, 'notes', lenient(integer, 0, u)),
    stop_at_ms: field(f, path, 'stop_at_ms', soft(nullable(number), null, u)),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readCheckParticipant(v, p, u), u),
    ),
    networks: field(
      f,
      path,
      'networks',
      items((v, p) => readCheckNetwork(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
    scenario: field(
      f,
      path,
      'scenario',
      soft(
        nullable((v, p) => readCheckScenario(v, p, u)),
        null,
        u,
      ),
    ),
  });
};

// --- Launcher: run result (same shape as run --json) ---------------------------------------------

export interface RunParticipant extends FromLauncher {
  name: string;
  required: boolean;
  enabled: boolean;
  /**
   * A participant the user started (or is to start). ended stays not_started (the joined event tells whether it
   * joined).
   */
  manual: boolean;
  ended: Open<'exited' | 'stopped' | 'killed' | 'not_started'>;
  exit_code: number | null;
  signal: string | null;
  left_processes: boolean;
  /** State of the connection to the ECU's variables at the start (null from older launchers, which do not write it). */
  control_link: ControlLink | null;
  /**
   * The file the participant's output was written to (`--log-dir`). null without `--log-dir`, for a participant that
   * was not started, or from an older launcher (absent on an object built by hand).
   */
  log?: string | null;
}

/**
 * State of the connection to the ECU's variables (the launcher's control links). The launcher's words appear only here
 * (unknown values pass through as strings, in case names are added or changed).
 * connected: variables can be read and written; no_hello: the ECU declared that it exposes variables but no hello
 * came; version_mismatch: the format versions differ; not_offered: the ECU has variables but did not declare that it
 * exposes them (--no-control, an older executable); none: not an ECU that exposes variables.
 */
export type ControlState = Open<
  'connected' | 'no_hello' | 'version_mismatch' | 'not_offered' | 'none'
>;

/** Name of the launcher event (state of the connection to the ECU's variables). */
export const CONTROL_EVENT = 'control_link';
/** Key (an object) for the variable interface's connection in participants[] of the launcher's result. */
export const CONTROL_RESULT_KEY = 'control_link';
/**
 * Field for the variable interface's connection in participants[] of the GUI server's summary (the event fields laid
 * over each other).
 */
export const CONTROL_SUMMARY_KEY = 'control_link';
/** Field in participants[] of the check result for how the ECU declared it exposes its variables. */
export const CONTROL_CHECK_KEY = 'control';

/** How the ECU declared it exposes its variables (participants[].control of the check). */
export interface ControlOffer extends FromLauncher {
  /** Transport (silkit for now). */
  transport: string;
  /** Format version of the messages. */
  wire_version: number | null;
}

const readControlOffer = (value: unknown, path: string, u?: Unreadable): ControlOffer => {
  const f = object(value, path);
  return withRaw(f, {
    transport: field(f, path, 'transport', lenient(string, '', u)),
    wire_version: field(f, path, 'wire_version', soft(nullable(integer), null, u)),
  });
};

/**
 * The launcher's control_link event and result (and control_link of a participant in the summary): how a participant's
 * variable interface is connected.
 */
export interface ControlLink extends FromLauncher {
  state: ControlState;
  /** connected: number of variables the ECU exposes. */
  variables: number | null;
  /** version_mismatch: the other side's format version. */
  version: number | null;
  /** Number of messages that arrived later than promised (result only; events lack it, so null). */
  late_messages: number | null;
}

export const readControlLink = (value: unknown, path: string, u?: Unreadable): ControlLink => {
  const f = object(value, path);
  return withRaw(f, {
    state: field(f, path, 'state', string),
    variables: field(f, path, 'variables', soft(nullable(integer), null, u)),
    version: field(f, path, 'version', soft(nullable(integer), null, u)),
    late_messages: field(f, path, 'late_messages', soft(nullable(integer), null, u)),
  });
};

const readRunParticipant = (value: unknown, path: string, u?: Unreadable): RunParticipant => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', string),
    required: field(f, path, 'required', lenient(boolean, true, u)),
    enabled: field(f, path, 'enabled', lenient(boolean, true, u)),
    manual: field(f, path, 'manual', soft(boolean, false, u)),
    ended: field(f, path, 'ended', lenient(string, '', u)),
    exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
    signal: field(f, path, 'signal', soft(nullable(string), null, u)),
    left_processes: field(f, path, 'left_processes', soft(boolean, false, u)),
    control_link: field(
      f,
      path,
      CONTROL_RESULT_KEY,
      soft(
        nullable((v, p) => readControlLink(v, p, u)),
        null,
        u,
      ),
    ),
    log: field(f, path, 'log', soft(nullable(string), null, u)),
  });
};

/** Name of the run result (`result.result` of the `result` event). */
export type RunResultName = Open<
  'passed' | 'failed' | 'interrupted' | 'check_failed' | 'unreadable' | 'setup_failed' | 'usage'
>;

/**
 * The result's timing: how time advanced (the numbers are defined in "Running at a set speed (`speed`)" in
 * docs/simulation-run.md). null in the result when nothing was started, or from an older launcher. Each field is null
 * when it is missing or cannot be read.
 */
export interface RunTiming extends FromLauncher {
  /** Speed at the end. */
  speed: Speed;
  /** The controller's time step (ms). A step command is rounded up to a multiple of it. */
  time_step_ms: number | null;
  ecu_step_ms: number | null;
  /** Maximum by which an ECU step runs late because the time step is coarser than the ECU's step (ms). */
  step_delay_ms: number | null;
  late_tolerance_ms: number | null;
  sim_ms: number | null;
  /** Real time taken (including time spent paused). */
  wall_ms: number | null;
  actual_speed: number | null;
  /** Number of paced time steps. 0 means it ran as fast as possible (lateness not counted). */
  paced_steps: number | null;
  late_steps: number | null;
  max_late_ms: number | null;
  slipped_ms: number | null;
  late_ms: number | null;
  paused_ms: number | null;
}

const readRunTiming = (value: unknown, path: string, u?: Unreadable): RunTiming => {
  const f = object(value, path);
  const count = (key: string) => field(f, path, key, soft(nullable(number), null, u));
  return withRaw(f, {
    speed: field(f, path, 'speed', soft(readSpeed, null, u)),
    time_step_ms: count('time_step_ms'),
    ecu_step_ms: count('ecu_step_ms'),
    step_delay_ms: count('step_delay_ms'),
    late_tolerance_ms: count('late_tolerance_ms'),
    sim_ms: count('sim_ms'),
    wall_ms: count('wall_ms'),
    actual_speed: count('actual_speed'),
    paced_steps: count('paced_steps'),
    late_steps: count('late_steps'),
    max_late_ms: count('max_late_ms'),
    slipped_ms: count('slipped_ms'),
    late_ms: count('late_ms'),
    paused_ms: count('paused_ms'),
  });
};

// The traffic of the run result: "Network traffic (`traffic:`)" in docs/simulation-run.md. The words of not_counted
// and reason may change, so they are plain strings.

/** A participant of the run that the launcher does not know to be on a network (or on any network). */
export interface TrafficUnplaced extends FromLauncher {
  participant: string;
  /** Why (for example no_control_link, no_counters). */
  reason: string;
}

const readTrafficUnplaced = (value: unknown, path: string, u?: Unreadable): TrafficUnplaced => {
  const f = object(value, path);
  return withRaw(f, {
    participant: field(f, path, 'participant', string),
    reason: field(f, path, 'reason', lenient(string, '', u)),
  });
};

/** The counts of one participant on one network. */
export interface TrafficParticipant extends FromLauncher {
  name: string;
  counted: boolean;
  /** Why it was not counted (for example no_control_link, restarted, real_time); null when counted. */
  not_counted: string | null;
  /** Whether it runs in virtual time; null if its connection to the ECU variables did not connect. */
  virtual_time: boolean | null;
  /** Virtual time of the step whose counts were read (ms); null when not counted. */
  at_ms: number | null;
  /** Frames the port accepted for sending; null when not counted. */
  frames_sent: number | null;
  /** J1939 frames of other participants that the port delivered; null when not counted. */
  frames_received: number | null;
}

const readTrafficParticipant = (
  value: unknown,
  path: string,
  u?: Unreadable,
): TrafficParticipant => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', string),
    counted: field(f, path, 'counted', lenient(boolean, false, u)),
    not_counted: field(f, path, 'not_counted', soft(nullable(string), null, u)),
    virtual_time: field(f, path, 'virtual_time', soft(nullable(boolean), null, u)),
    at_ms: field(f, path, 'at_ms', soft(nullable(number), null, u)),
    frames_sent: field(f, path, 'frames_sent', soft(nullable(integer), null, u)),
    frames_received: field(f, path, 'frames_received', soft(nullable(integer), null, u)),
  });
};

/** The traffic on one network of the configuration file. */
export interface TrafficNetwork extends FromLauncher {
  /** The simulation's name of the network. */
  name: string;
  kind: string | null;
  /** Number of counted participants. */
  counted: number;
  /** How many counted participants sent one frame or more. */
  senders: number;
  /** How many counted participants received one frame or more. */
  receivers: number;
  /** true: sent frames arrived; false: a counted participant sent and none received them; null: cannot be decided. */
  delivered: boolean | null;
  /** Participants that may be on this network without the launcher knowing. */
  unplaced: TrafficUnplaced[];
  /** The participants known to be on the network, in configuration file order. */
  participants: TrafficParticipant[];
}

const readTrafficNetwork = (value: unknown, path: string, u?: Unreadable): TrafficNetwork => {
  const f = object(value, path);
  return withRaw(f, {
    name: field(f, path, 'name', string),
    kind: field(f, path, 'kind', soft(nullable(string), null, u)),
    counted: field(f, path, 'counted', lenient(integer, 0, u)),
    senders: field(f, path, 'senders', lenient(integer, 0, u)),
    receivers: field(f, path, 'receivers', lenient(integer, 0, u)),
    delivered: field(f, path, 'delivered', soft(nullable(boolean), null, u)),
    unplaced: field(
      f,
      path,
      'unplaced',
      items((v, p) => readTrafficUnplaced(v, p, u), u),
    ),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readTrafficParticipant(v, p, u), u),
    ),
  });
};

/** `traffic` of the run result: whether J1939 frames arrived on each network. */
export interface RunTraffic extends FromLauncher {
  /** The virtual time at which the run ended; the counts are from steps that started before it (ms). */
  before_ms: number | null;
  /** Participants of the run that the launcher knows on no network. */
  unplaced: TrafficUnplaced[];
  /** Every network of the configuration file, in its order. */
  networks: TrafficNetwork[];
}

const readRunTraffic = (value: unknown, path: string, u?: Unreadable): RunTraffic => {
  const f = object(value, path);
  return withRaw(f, {
    before_ms: field(f, path, 'before_ms', soft(nullable(number), null, u)),
    unplaced: field(
      f,
      path,
      'unplaced',
      items((v, p) => readTrafficUnplaced(v, p, u), u),
    ),
    networks: field(
      f,
      path,
      'networks',
      items((v, p) => readTrafficNetwork(v, p, u), u),
    ),
  });
};

/** Result of `vehicle_sil_launch run --json` (format version 1). */
export interface RunResult extends FromLauncher {
  file: string | null;
  result: RunResultName;
  errors: number;
  warnings: number;
  notes: number;
  simulation: string | null;
  interrupted_by: string | null;
  /** How time advanced. null from older launchers, and when nothing was started. */
  timing: RunTiming | null;
  participants: RunParticipant[];
  findings: Finding[];
  check: CheckResult | null;
  /** Result of the scenario (only for a run with `--scenario`; null otherwise). */
  scenario: ScenarioResult | null;
  /**
   * Whether J1939 frames arrived on each network. null if the simulation did not start, from an older launcher, or in
   * a result the server shrank (absent on an object built by hand).
   */
  traffic?: RunTraffic | null;
}

/**
 * Reads a run result. With u, part by part: a result the server shrank to fit its limits (only the short, non-nested
 * fields) is still read as a result (participants and findings become empty and go into the unreadable parts). ''
 * if the result's name cannot be read.
 */
export const readRunResult = (value: unknown, path: string, u?: Unreadable): RunResult => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_run', 1, u);
  return withRaw(f, {
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    result: field(f, path, 'result', lenient(string, '', u)),
    errors: field(f, path, 'errors', lenient(integer, 0, u)),
    warnings: field(f, path, 'warnings', lenient(integer, 0, u)),
    notes: field(f, path, 'notes', lenient(integer, 0, u)),
    simulation: field(f, path, 'simulation', soft(nullable(string), null, u)),
    interrupted_by: field(f, path, 'interrupted_by', soft(nullable(string), null, u)),
    timing: field(
      f,
      path,
      'timing',
      soft(
        nullable((v, p) => readRunTiming(v, p, u)),
        null,
        u,
      ),
    ),
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readRunParticipant(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
    check: field(
      f,
      path,
      'check',
      soft(
        nullable((v, p) => readCheckResult(v, p, u)),
        null,
        u,
      ),
    ),
    scenario: field(
      f,
      path,
      'scenario',
      soft(
        nullable((v, p) => readScenarioResult(v, p, u)),
        null,
        u,
      ),
    ),
    traffic: field(
      f,
      path,
      'traffic',
      soft(
        nullable((v, p) => readRunTraffic(v, p, u)),
        null,
        u,
      ),
    ),
  });
};

// --- Launcher: progress events (one line of run --events) ----------------------------------------

/** Phases. They advance in this order; one may be skipped, but they never go back. */
export const PHASES = ['checking', 'starting', 'waiting', 'running', 'stopping', 'ended'] as const;
export type Phase = Open<(typeof PHASES)[number]>;

export type ParticipantState = Open<
  | 'disabled'
  | 'not_started'
  | 'started'
  /** Not started because the user starts this participant (instead of started). joined follows once it joins. */
  | 'manual'
  | 'joined'
  | 'failed'
  | 'ended'
  | 'stop_requested'
  | 'killed'
>;

interface EventBase extends FromLauncher {
  seq: number;
  /** Real time since the launcher started (ms). Some events lack it (the server passes them through without it). */
  t_ms: number | null;
  /**
   * Names of the fields that could not be read (prefixed with the event kind, as in `finding.code`). Absent if none.
   */
  unreadable?: string[];
}

export type LauncherEvent =
  | (EventBase & {
      event: 'hello';
      vehicle_sil_events: number;
      vehicle_sil: string;
      pid: number | null;
      file: string | null;
      arguments: string[];
    })
  | (EventBase & { event: 'phase'; phase: Phase })
  | (EventBase & { event: 'check'; check: CheckResult | null })
  | (EventBase & { event: 'registry'; uri: string | null })
  | (EventBase & {
      event: 'participant';
      name: string;
      state: ParticipantState;
      pid: number | null;
      exit_code: number | null;
      signal: string | null;
      /**
       * failed: why it was counted as failed (left_simulation, process_ended, ended_before_start; more may be added).
       */
      reason: string | null;
      /** not_started: why it could not be started. failed: the text of that finding (if decided). */
      message: string | null;
      /** manual: the command that starts it, as one line to paste into this OS's shell (as the launcher wrote it). */
      command_line: string | null;
      /** manual: the directory to run that command in. */
      working_directory: string | null;
      /**
       * manual: environment variables to set when starting it (names and values). Only when the registry is not on a
       * loopback address (`{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}`). null if none.
       */
      environment: Record<string, string> | null;
    })
  | (EventBase & { event: 'log'; source: string; line: string })
  /** State of the connection to the ECU's variables (once per started participant, just before the start). */
  | (EventBase & { event: typeof CONTROL_EVENT; name: string; link: ControlLink })
  /**
   * Number of log lines the launcher could not write because the reader was slow (per source). Arrives at their
   * position.
   */
  | (EventBase & { event: 'log_dropped'; count: number; sources: Record<string, number> })
  | (EventBase & { event: 'finding'; finding: Finding })
  | (EventBase & { event: 'time' } & TimeReport)
  | (EventBase & { event: 'command' } & CommandReport)
  /** What the run accepts now (docs/simulation-events.md): written whenever that changes. */
  | (EventBase & { event: 'state' } & StateReport)
  /** A scenario step started (status running) or was decided. */
  | (EventBase & { event: 'scenario_step'; step: ScenarioStep })
  | (EventBase & { event: 'result'; result: RunResult });

/**
 * The time event (and time of the summary): the virtual time the controller sees, and how it keeps up with real time.
 * Whether the simulation is paused, and the speed set, are in the state event (StateReport). Fields that are absent
 * or unreadable are null (the screen does not show null values).
 */
export interface TimeReport {
  /** Virtual time (ms; never decreases). */
  sim_ms: number;
  /** Actual speed over roughly the last second. null when it has run only briefly, or in an event written while paused. */
  actual_speed: number | null;
  /** Number of late time steps so far (the value if it ended now; 0 when running as fast as possible). */
  late_steps: number | null;
  /** Max lateness so far (ms). */
  max_late_ms: number | null;
  /** Total slip so far (ms). */
  slipped_ms: number | null;
  /** Current lateness of a time step that is past its deadline and has not started yet (ms; 0 if none). */
  late_ms: number | null;
}

export function readTimeFields(f: Fields, path: string, u?: Unreadable): TimeReport {
  const count = (key: string) => field(f, path, key, soft(nullable(number), null, u));
  return {
    sim_ms: field(f, path, 'sim_ms', number),
    actual_speed: count('actual_speed'),
    late_steps: count('late_steps'),
    max_late_ms: count('max_late_ms'),
    slipped_ms: count('slipped_ms'),
    late_ms: count('late_ms'),
  };
}

/** The command event (and last_command of the summary): whether the launcher accepted a command from stdin. */
export interface CommandReport {
  /** Name of the command (null for an unreadable line). */
  command: string | null;
  /** stdin: arrived as a line; eof: end of stdin. */
  source: Open<'stdin' | 'eof'>;
  accepted: boolean;
  /** Why it was not accepted (shown in English, unchanged). null if accepted. */
  message: string | null;
  /**
   * For an accepted step, the virtual time it actually advances (rounded up to a multiple of the time step). null
   * otherwise.
   */
  ms: number | null;
  /** Why it was not accepted, as a code that stays the same (an entry of `reason` in the launcher's documents). null if accepted. */
  reason: string | null;
  /**
   * The `id` the line had (the GUI server passes the `id` of POST api/time on), so an answer is tied to the command it
   * answers. null when the line had none, and from a launcher before `state`.
   */
  id: number | null;
  /**
   * For an accepted `step`, `run_until` and `pause` with a time: the virtual time at which the simulation pauses (null
   * otherwise, and from a launcher that does not write it).
   */
  at_ms: number | null;
}

/**
 * last_command of the summary: the event's fields and that event's seq (null from older servers, which do not write
 * it).
 */
export type SummaryCommand = CommandReport & { seq: number | null };

export function readCommandFields(f: Fields, path: string, u?: Unreadable): CommandReport {
  return {
    command: field(f, path, 'command', nullable(string)),
    source: field(f, path, 'source', soft(string, 'stdin', u)),
    accepted: field(f, path, 'accepted', boolean),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
    ms: field(f, path, 'ms', soft(nullable(integer), null, u)),
    reason: field(f, path, 'reason', soft(nullable(string), null, u)),
    id: field(f, path, 'id', soft(nullable(integer), null, u)),
    at_ms: field(f, path, 'at_ms', soft(nullable(number), null, u)),
  };
}

/** A reason with an English sentence: the `caution` of an operation the launcher accepts with a warning. */
export interface Caution {
  code: string;
  message: string;
}

/**
 * Whether the launcher accepts one operation now (an entry of `allowed` in the `state` event). `ok` says "it would
 * accept it", not "it would change something". The other keys of the entry (`min_ms`, `max_ms` of step, `min`, `max`
 * of speed) are in `raw`.
 */
export interface LauncherAllowedItem extends FromLauncher {
  ok: boolean;
  /** Why not: a code that stays the same. null if ok. */
  reason: string | null;
  /** Why not, in one English sentence. null if ok. */
  message: string | null;
  /** Accepted, with a warning. null if none. */
  caution: Caution | null;
}

const readLauncherAllowedItem: Reader<LauncherAllowedItem> = (value, path) => {
  const f = object(value, path);
  return withRaw(f, {
    ok: field(f, path, 'ok', boolean),
    reason: field(f, path, 'reason', soft(nullable(string), null)),
    message: field(f, path, 'message', soft(nullable(string), null)),
    caution: field(
      f,
      path,
      'caution',
      soft(
        nullable((v, p): Caution => {
          const c = object(v, p);
          return { code: field(c, p, 'code', string), message: field(c, p, 'message', string) };
        }),
        null,
      ),
    ),
  });
};

/**
 * The state event: where the run stands and what it accepts now. Keys not named here (`outside`) are in `raw`. The
 * set of keys of `allowed` can grow.
 */
export interface StateReport {
  /** Goes up with each change: tells a newer state from the same one sent again. */
  rev: number;
  /** The phase (as in the phase event). */
  phase: string;
  /** Whether the simulation stands still. null if not written. */
  paused: boolean | null;
  /**
   * The virtual time (ms) at which it stands still. null while it does not, and while `paused` is true but the
   * simulation has not come to a stand yet (before it starts; a pause at a time that has passed, until the next step).
   */
  paused_at_ms: number | null;
  /** Whether the run waits before the step at time 0 (a run started paused with a Control Server). null if not written. */
  holding: boolean | null;
  /** The speed that was asked for. null before the launcher knows it. */
  speed: Speed;
  /** Whether the run keeps to its speed: not-paced, on-time or late. */
  pacing: Open<'not-paced' | 'on-time' | 'late'> | null;
  /** The stop requests the run has had, and the first source. */
  stop: { requests: number; by: string | null } | null;
  /** Whether the run plays a scenario. */
  scenario: boolean | null;
  /** The operations: pause, resume, step, speed, run_until, stop, write. */
  allowed: Record<string, LauncherAllowedItem>;
}

export function readStateFields(f: Fields, path: string, u?: Unreadable): StateReport {
  return {
    rev: field(f, path, 'rev', integer),
    phase: field(f, path, 'phase', lenient(string, '', u)),
    paused: field(f, path, 'paused', soft(nullable(boolean), null, u)),
    paused_at_ms: field(f, path, 'paused_at_ms', soft(nullable(number), null, u)),
    holding: field(f, path, 'holding', soft(nullable(boolean), null, u)),
    speed: field(f, path, 'speed', soft(readSpeed, null, u)),
    pacing: field(f, path, 'pacing', soft(nullable(string), null, u)),
    stop: field(
      f,
      path,
      'stop',
      soft(
        nullable((v, p) => {
          const s = object(v, p);
          return {
            requests: field(s, p, 'requests', integer),
            by: field(s, p, 'by', nullable(string)),
          };
        }),
        null,
        u,
      ),
    ),
    scenario: field(f, path, 'scenario', soft(nullable(boolean), null, u)),
    allowed: field(f, path, 'allowed', lenient(record(readLauncherAllowedItem), {}, u)),
  };
}

/** The last state event, as the summary of the session keeps it: with that event's seq. */
export type SummaryState = StateReport & { seq: number | null };

/**
 * Reads one launcher event. An unknown event is null (skipped). If partly is true, reads part by part: an unreadable
 * field gets its default value and its name goes into unreadable (throws only when the core seq and event, or the core
 * fields of the event kind, cannot be read).
 */
export const readLauncherEvent = (
  value: unknown,
  path: string,
  partly = false,
): LauncherEvent | null => {
  const f = object(value, path);
  const kind = field(f, path, 'event', string);
  const u = partly ? new Unreadable(path, `${kind}.`) : undefined;
  const event = readEventFields(
    f,
    path,
    kind,
    {
      seq: field(f, path, 'seq', integer),
      t_ms: field(f, path, 't_ms', soft(nullable(number), null, u)),
    },
    u,
  );
  if (event && u && u.parts.length > 0) event.unreadable = u.parts;
  return event && withRaw(f, event);
};

function readEventFields(
  f: Fields,
  path: string,
  kind: string,
  base: EventBase,
  u?: Unreadable,
): LauncherEvent | null {
  switch (kind) {
    case 'hello':
      // The server detects and reports a format version mismatch. The screen uses only the readable fields
      return {
        ...base,
        event: kind,
        vehicle_sil_events: field(f, path, 'vehicle_sil_events', lenient(integer, 0, u)),
        vehicle_sil: field(f, path, 'vehicle_sil', lenient(string, '', u)),
        pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
        file: field(f, path, 'file', soft(nullable(string), null, u)),
        arguments: field(f, path, 'arguments', soft(items(string, u), [], u)),
      };
    case 'phase':
      return { ...base, event: kind, phase: field(f, path, 'phase', string) };
    case 'check':
      // An unreadable check is null (reported as an unreadable part; the other events are still applied)
      return {
        ...base,
        event: kind,
        check: field(
          f,
          path,
          'check',
          lenient((v, p) => readCheckResult(v, p, u), null, u),
        ),
      };
    case 'registry':
      return { ...base, event: kind, uri: field(f, path, 'uri', soft(nullable(string), null, u)) };
    case 'participant':
      return {
        ...base,
        event: kind,
        name: field(f, path, 'name', string),
        state: field(f, path, 'state', string),
        pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
        exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
        signal: field(f, path, 'signal', soft(nullable(string), null, u)),
        reason: field(f, path, 'reason', soft(nullable(string), null, u)),
        message: field(f, path, 'message', soft(nullable(string), null, u)),
        command_line: field(f, path, 'command_line', soft(nullable(string), null, u)),
        working_directory: field(f, path, 'working_directory', soft(nullable(string), null, u)),
        environment: field(f, path, 'environment', soft(nullable(record(string)), null, u)),
      };
    case 'log_dropped':
      return {
        ...base,
        event: kind,
        count: field(f, path, 'count', integer),
        sources: field(f, path, 'sources', record(integer)),
      };
    case CONTROL_EVENT:
      return {
        ...base,
        event: kind,
        name: field(f, path, 'name', string),
        link: readControlLink(f, path, u),
      };
    case 'log':
      return {
        ...base,
        event: kind,
        source: field(f, path, 'source', string),
        line: field(f, path, 'line', string),
      };
    case 'finding':
      // The finding's fields sit directly in the event line
      return { ...base, event: kind, finding: readFinding(f, path, u) };
    case 'time':
      return { ...base, event: kind, ...readTimeFields(f, path, u) };
    case 'command':
      return { ...base, event: kind, ...readCommandFields(f, path, u) };
    case 'state':
      return { ...base, event: kind, ...readStateFields(f, path, u) };
    case 'scenario_step':
      // The step's fields sit directly in the event line
      return { ...base, event: kind, step: readScenarioStepFields(f, path, u) };
    case 'result':
      return {
        ...base,
        event: kind,
        result: field(f, path, 'result', (v, p) => readRunResult(v, p, u)),
      };
    default:
      return null;
  }
}
