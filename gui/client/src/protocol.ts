/**
 * Shapes of the data exchanged with the server, and their parsers (gui/protocol/ is the authority).
 *
 * The types of the server API are written here only; users of the library (the screens too) import them from the
 * entry point (index.ts). JSON that arrives (HTTP answers, WebSocket notifications) is used only after a read*
 * function here has checked its type.
 * - Unknown fields are ignored. Unknown events and unknown notifications become null and are skipped.
 * - Fields whose set of values may grow (phase, state, result and so on) pass unknown values through as strings.
 * - Data of the wrong shape (missing field, wrong type, unknown format version) throws ProtocolError.
 * - Exception: the session summary and launcher events are read part by part (readSessionSummary, readServerMessage).
 *   If a part that comes from the launcher (check, result, findings, participants) or a part the server shrank to fit
 *   its limits cannot be read, the readable parts are still used; an unreadable part gets its default value and its
 *   name goes into `unreadable`. Only when the summary core (id, state, launcher, stop) or the event core (seq, event)
 *   cannot be read is the whole thing unreadable.
 * The unit tests (protocol.spec.ts) feed every example in gui/protocol/examples/ through these parsers.
 */

/** Data of an unreadable shape. path is where it broke (for example `event.participants[0].name`). */
export class ProtocolError extends Error {
  constructor(
    readonly path: string,
    readonly expected: string,
  ) {
    super(`${path}: expected ${expected}`);
    this.name = 'ProtocolError';
  }
}

/** A string whose set of values may grow. Known values get completion; unknown values are allowed too. */
export type Open<T extends string> = T | (string & {});

/**
 * The version of the GUI server's API that this library reads (`vehicle_sil_gui` of `api/app` and `hello`). It does not
 * read a hello or an api/app of another version (their fields may mean something else). When to raise the version:
 * "Versioning and compatibility" in gui/protocol/README.md.
 */
export const PROTOCOL_VERSION = 1;

// --- Parsing helpers -----------------------------------------------------------------------------

/**
 * Reads a received value. path is how the error location is shown (for example `message.session`). Throws ProtocolError
 * on the wrong shape.
 */
export type Reader<T> = (value: unknown, path: string) => T;
type Fields = Record<string, unknown>;

function object(value: unknown, path: string): Fields {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new ProtocolError(path, 'an object');
  }
  return value as Fields;
}

const string: Reader<string> = (value, path) => {
  if (typeof value !== 'string') throw new ProtocolError(path, 'a string');
  return value;
};

const number: Reader<number> = (value, path) => {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new ProtocolError(path, 'a number');
  }
  return value;
};

const integer: Reader<number> = (value, path) => {
  if (!Number.isInteger(value)) throw new ProtocolError(path, 'an integer');
  return value as number;
};

const boolean: Reader<boolean> = (value, path) => {
  if (typeof value !== 'boolean') throw new ProtocolError(path, 'true or false');
  return value;
};

function nullable<T>(read: Reader<T>): Reader<T | null> {
  return (value, path) => (value === null ? null : read(value, path));
}

/** fallback when the field is absent (undefined). For fields that older launchers do not write yet. */
function optional<T, F>(read: Reader<T>, fallback: F): Reader<T | F> {
  return (value, path) => (value === undefined ? fallback : read(value, path));
}

function array<T>(read: Reader<T>): Reader<T[]> {
  return (value, path) => {
    if (!Array.isArray(value)) throw new ProtocolError(path, 'an array');
    return value.map((item, i) => read(item, `${path}[${i}]`));
  };
}

function field<T>(fields: Fields, path: string, key: string, read: Reader<T>): T {
  return read(fields[key], `${path}.${key}`);
}

/**
 * Collects the names of the parts that could not be read when reading part by part. Names are relative to root,
 * without array indexes (`message.session.participants[2].signal` → `participants.signal`).
 */
export class Unreadable {
  private readonly names = new Set<string>();

  constructor(
    private readonly root: string,
    private readonly prefix = '',
  ) {}

  add(path: string): void {
    const relative = path.startsWith(`${this.root}.`) ? path.slice(this.root.length + 1) : path;
    this.names.add(this.prefix + relative.replace(/\[\d+\]/g, ''));
  }

  get parts(): string[] {
    return [...this.names];
  }
}

/**
 * If unreadable, uses fallback and records the location in u (throws without u). An absent field counts as unreadable.
 */
function lenient<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  if (!u) return read;
  return (value, path) => {
    try {
      return read(value, path);
    } catch (error) {
      if (!(error instanceof ProtocolError)) throw error;
      u.add(error.path);
      return fallback;
    }
  };
}

/**
 * An absent field silently becomes fallback; a field of the wrong type becomes fallback and is recorded in u (throws
 * without u).
 */
function soft<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  return lenient(optional(read, fallback), fallback, u);
}

/** An array. With u, unreadable elements are skipped and recorded in u (empty if absent or not an array). */
function items<T>(read: Reader<T>, u?: Unreadable): Reader<T[]> {
  if (!u) return array(read);
  return (value, path) => {
    if (!Array.isArray(value)) {
      u.add(path);
      return [];
    }
    const out: T[] = [];
    value.forEach((item, i) => {
      try {
        out.push(read(item, `${path}[${i}]`));
      } catch (error) {
        if (!(error instanceof ProtocolError)) throw error;
        u.add(error.path);
      }
    });
    return out;
  };
}

/**
 * A format version field. An unknown version is not read (its meaning may have changed). When reading part by part
 * (u), it is recorded as an unreadable part and the readable fields are still read.
 */
function version(
  fields: Fields,
  path: string,
  key: string,
  expected: number,
  u?: Unreadable,
): void {
  if (fields[key] !== expected) {
    const error = new ProtocolError(`${path}.${key}`, `format version ${expected}`);
    if (!u) throw error;
    u.add(error.path);
  }
}

// --- Launcher: check (same shape as check --json) ------------------------------------------------

/** Severity of a finding. An unknown severity is treated as an error (severityRank). */
export type Severity = Open<'error' | 'warning' | 'note'>;

/** A finding of the check or the run. message and detail are shown in English, unchanged. */
export interface Finding {
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
  return {
    severity: field(f, path, 'severity', string),
    code: field(f, path, 'code', lenient(string, '', u)),
    file: field(f, path, 'file', soft(nullable(string), null, u)),
    line: field(f, path, 'line', soft(nullable(integer), null, u)),
    participant: field(f, path, 'participant', soft(nullable(string), null, u)),
    network: field(f, path, 'network', soft(nullable(string), null, u)),
    message: field(f, path, 'message', string),
    detail: field(f, path, 'detail', soft(nullable(string), null, u)),
  };
};

/** A variable the ECU declared about itself (so panels can be drawn before the run). */
export interface Variable {
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
  return {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', string),
    causality: field(f, path, 'causality', string),
    unit: field(f, path, 'unit', optional(string, '')),
    min: field(f, path, 'min', optional(nullable(number), null)),
    max: field(f, path, 'max', optional(nullable(number), null)),
    builtin: field(f, path, 'builtin', optional(nullable(boolean), null)),
  };
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
export interface ScenarioStep {
  /** From 1. */
  index: number;
  /** Line in the scenario file. */
  line: number | null;
  kind: Open<'set' | 'ramp' | 'wave' | 'expect' | 'wait_until'>;
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

const readScenarioStepFields = (f: Fields, path: string, u?: Unreadable): ScenarioStep => ({
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

const readScenarioStep = (value: unknown, path: string, u?: Unreadable): ScenarioStep =>
  readScenarioStepFields(object(value, path), path, u);

/** `scenario` of the check result (only with `--scenario`). */
export interface CheckScenario {
  /** The scenario file (as passed to the launcher). */
  file: string;
  name: string | null;
  /** Number of steps. */
  steps: number;
}

const readCheckScenario = (value: unknown, path: string, u?: Unreadable): CheckScenario => {
  const f = object(value, path);
  return {
    file: field(f, path, 'file', lenient(string, '', u)),
    name: field(f, path, 'name', soft(nullable(string), null, u)),
    steps: field(f, path, 'steps', lenient(integer, 0, u)),
  };
};

/** `scenario` of the run result (only with `--scenario`). */
export interface ScenarioResult {
  file: string;
  name: string | null;
  status: ScenarioStatus;
  /** The controller step at which the scenario ended (passed and failed; null otherwise). */
  over_at_ms: number | null;
  steps: ScenarioStep[];
}

const readScenarioResult = (value: unknown, path: string, u?: Unreadable): ScenarioResult => {
  const f = object(value, path);
  return {
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
  };
};

export type CheckStatus = Open<'described' | 'not_checked' | 'failed' | 'skipped' | 'disabled'>;

export interface CheckParticipant {
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
}

const readCheckParticipant = (value: unknown, path: string, u?: Unreadable): CheckParticipant => {
  const f = object(value, path);
  return {
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
  };
};

export interface CheckNetwork {
  name: string | null;
  kind: string | null;
  /** Participant names in use (in configuration file order). */
  participants: string[];
}

const readCheckNetwork = (value: unknown, path: string, u?: Unreadable): CheckNetwork => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', lenient(nullable(string), null, u)),
    kind: field(f, path, 'kind', lenient(nullable(string), null, u)),
    participants: field(f, path, 'participants', items(string, u)),
  };
};

export type CheckOutcome = Open<'passed' | 'failed' | 'unreadable' | 'usage'>;

/** Result of `vehicle_sil_launch check --json` (format version 1). */
export interface CheckResult {
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
  return {
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
  };
};

// --- Launcher: run result (same shape as run --json) ---------------------------------------------

export interface RunParticipant {
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
export interface ControlOffer {
  /** Transport (silkit for now). */
  transport: string;
  /** Format version of the messages. */
  wire_version: number | null;
}

const readControlOffer = (value: unknown, path: string, u?: Unreadable): ControlOffer => {
  const f = object(value, path);
  return {
    transport: field(f, path, 'transport', lenient(string, '', u)),
    wire_version: field(f, path, 'wire_version', soft(nullable(integer), null, u)),
  };
};

/**
 * The launcher's control_link event and result (and control_link of a participant in the summary): how a participant's
 * variable interface is connected.
 */
export interface ControlLink {
  state: ControlState;
  /** connected: number of variables the ECU exposes. */
  variables: number | null;
  /** version_mismatch: the other side's format version. */
  version: number | null;
  /** Number of messages that arrived later than promised (result only; events lack it, so null). */
  late_messages: number | null;
}

const readControlLink = (value: unknown, path: string, u?: Unreadable): ControlLink => {
  const f = object(value, path);
  return {
    state: field(f, path, 'state', string),
    variables: field(f, path, 'variables', soft(nullable(integer), null, u)),
    version: field(f, path, 'version', soft(nullable(integer), null, u)),
    late_messages: field(f, path, 'late_messages', soft(nullable(integer), null, u)),
  };
};

const readRunParticipant = (value: unknown, path: string, u?: Unreadable): RunParticipant => {
  const f = object(value, path);
  return {
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
  };
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
export interface RunTiming {
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
  return {
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
  };
};

/** Result of `vehicle_sil_launch run --json` (format version 1). */
export interface RunResult {
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
}

/**
 * Reads a run result. With u, part by part: a result the server shrank to fit its limits (only the short, non-nested
 * fields) is still read as a result (participants and findings become empty and go into the unreadable parts). ''
 * if the result's name cannot be read.
 */
export const readRunResult = (value: unknown, path: string, u?: Unreadable): RunResult => {
  const f = object(value, path);
  version(f, path, 'vehicle_sil_run', 1, u);
  return {
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
  };
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

interface EventBase {
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
  /** A scenario step started (status running) or was decided. */
  | (EventBase & { event: 'scenario_step'; step: ScenarioStep })
  | (EventBase & { event: 'result'; result: RunResult });

/**
 * The time event (and time of the summary): the virtual time the controller sees, and how it advances. Older launchers
 * write only sim_ms, so the other fields are null when absent or unreadable (the screen does not show null values).
 */
export interface TimeReport {
  /** Virtual time (ms; never decreases). */
  sim_ms: number;
  /** The current speed (a number or 'max'). null if not written. */
  speed: Speed;
  /** Whether it is paused (true once it reaches the time it was to stop at). null if not written. */
  paused: boolean | null;
  /** Actual speed over roughly the last second. null when it has run only briefly, or while paused. */
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

function readTimeFields(f: Fields, path: string, u?: Unreadable): TimeReport {
  const count = (key: string) => field(f, path, key, soft(nullable(number), null, u));
  return {
    sim_ms: field(f, path, 'sim_ms', number),
    speed: field(f, path, 'speed', soft(readSpeed, null, u)),
    paused: field(f, path, 'paused', soft(nullable(boolean), null, u)),
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
}

/**
 * last_command of the summary: the event's fields and that event's seq (null from older servers, which do not write
 * it).
 */
export type SummaryCommand = CommandReport & { seq: number | null };

function readCommandFields(f: Fields, path: string, u?: Unreadable): CommandReport {
  return {
    command: field(f, path, 'command', nullable(string)),
    source: field(f, path, 'source', soft(string, 'stdin', u)),
    accepted: field(f, path, 'accepted', boolean),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
    ms: field(f, path, 'ms', soft(nullable(integer), null, u)),
  };
}

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
  return event;
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

// --- GUI server: common --------------------------------------------------------------------------

/**
 * A server notice (an API error, a WebSocket error, problems of the session). message is shown in English, unchanged.
 */
export interface ServerProblem {
  code: string;
  message: string;
  /** For launcher-stderr, the number of lines dropped at this point (`detail.skipped`). Other notices lack it. */
  skipped?: number;
}

/** Reads a server notice (`{code, message, detail}`). The `error` in an HTTP error has the same shape. */
export const readServerProblem: Reader<ServerProblem> = (value, path) => {
  const f = object(value, path);
  const problem: ServerProblem = {
    code: field(f, path, 'code', string),
    message: field(f, path, 'message', string),
  };
  // The contents of detail differ by code. Take only what is used, and only when its shape fits
  const detail = f['detail'];
  if (typeof detail === 'object' && detail !== null && 'skipped' in detail) {
    const skipped = detail.skipped;
    if (typeof skipped === 'number' && Number.isSafeInteger(skipped) && skipped >= 0) {
      problem.skipped = skipped;
    }
  }
  return problem;
};

/**
 * Speed. 'max' is as fast as possible; a number is a factor (1 is real-time speed). null means the launcher's default
 * (the value in the configuration file) when given at the start, and "not written" in a value the launcher writes.
 */
export type Speed = null | number | 'max';

const readSpeed: Reader<Speed> = (value, path) =>
  value === null || value === 'max' ? value : number(value, path);

// --- GUI server: HTTP API ------------------------------------------------------------------------

/** GET api/app: versions of the server and the launcher, whether they are usable, and limits. */
export interface AppInfo {
  version: string;
  /** An identifier that changes every time the server starts. If it changed, the server restarted. */
  instance: string;
  /** Where configuration files can be chosen from (for display). */
  root: string;
  /** The configuration file given at start-up (relative to root). null if none. */
  initial_file: string | null;
  launcher: {
    path: string | null;
    /** Whether the launcher has the features the screen needs. */
    usable: boolean;
    /** Why it is unusable (shown in English, unchanged). */
    problems: ServerProblem[];
    features: string[];
  };
  limits: {
    speed_min: number;
    speed_max: number;
    /** Upper limit of the ms of a step. Older servers do not write it, so the launcher's limit (10^12). */
    step_ms_max: number;
    /** Time limit of a check (s; `--check-timeout`). Older servers do not write it, so its default (120). */
    check_timeout_s: number;
    /**
     * Variable watches: lower and upper limit of the interval (ms), number per connection, variables that can be named
     * in one.
     */
    watch_period_ms_min: number;
    watch_period_ms_max: number;
    watches: number;
    watch_variables: number;
    /** Limit on the variables in one write, and limit on the virtual time (ms) a write can name. */
    writes_per_request: number;
    write_at_ms_max: number;
  };
  /**
   * Whether variables could be read and written if a run started now (the launcher has a Control Server and the GUI
   * server has gRPC), and why not (shown in English, unchanged). Older servers do not write it (available is false,
   * reason is null).
   */
  variables: { available: boolean; reason: string | null };
}

export const readAppInfo = (value: unknown): AppInfo => {
  const path = 'app';
  const f = object(value, path);
  version(f, path, 'vehicle_sil_gui', PROTOCOL_VERSION);
  const lp = `${path}.launcher`;
  const launcher = field(f, path, 'launcher', object);
  const launcherVersion = field(launcher, lp, 'version', nullable(object));
  const limits = field(f, path, 'limits', object);
  const lim = `${path}.limits`;
  const variables = field(f, path, 'variables', optional(object, null));
  return {
    version: field(f, path, 'version', string),
    instance: field(f, path, 'instance', string),
    root: field(f, path, 'root', string),
    initial_file: field(f, path, 'initial_file', nullable(string)),
    launcher: {
      path: field(launcher, lp, 'path', nullable(string)),
      usable: field(launcher, lp, 'usable', boolean),
      problems: field(launcher, lp, 'problems', array(readServerProblem)),
      features: launcherVersion
        ? field(launcherVersion, `${lp}.version`, 'features', optional(array(string), []))
        : [],
    },
    limits: {
      speed_min: field(limits, `${path}.limits`, 'speed_min', number),
      speed_max: field(limits, `${path}.limits`, 'speed_max', number),
      step_ms_max: field(limits, `${path}.limits`, 'step_ms_max', optional(number, 1e12)),
      check_timeout_s: field(limits, `${path}.limits`, 'check_timeout_s', optional(number, 120)),
      watch_period_ms_min: field(limits, lim, 'watch_period_ms_min', optional(number, 20)),
      watch_period_ms_max: field(limits, lim, 'watch_period_ms_max', optional(number, 60_000)),
      write_at_ms_max: field(limits, lim, 'write_at_ms_max', optional(number, 1e12)),
      watches: field(limits, lim, 'watches', optional(number, 16)),
      watch_variables: field(limits, lim, 'watch_variables', optional(number, 2000)),
      writes_per_request: field(limits, lim, 'writes_per_request', optional(number, 256)),
    },
    variables: variables
      ? {
          available: field(variables, `${path}.variables`, 'available', boolean),
          reason: field(variables, `${path}.variables`, 'reason', nullable(string)),
        }
      : { available: false, reason: null },
  };
};

/** One entry of GET api/configs. */
export interface ConfigFile {
  /** Path relative to root (separator /). */
  path: string;
}

/** GET api/configs: the configuration files under root. */
export interface ConfigList {
  files: ConfigFile[];
  /** Whether the list was cut off at the limit. */
  truncated: boolean;
}

export const readConfigList = (value: unknown): ConfigList => {
  const path = 'configs';
  const f = object(value, path);
  return {
    files: field(
      f,
      path,
      'files',
      array((item, p) => ({ path: field(object(item, p), p, 'path', string) })),
    ),
    truncated: field(f, path, 'truncated', boolean),
  };
};

/** Body of POST api/check. */
export interface CheckRequest {
  /** Path relative to root. */
  file: string;
  /** Names of the participants to leave out (only names in the check result). */
  disable: string[];
  /**
   * Names of the participants the user starts (only names in the check result that are not in disable). Send only when
   * the launcher's `features` include `manual-participants`.
   */
  manual?: string[];
  /**
   * The scenario (`path` from `GET api/scenarios`; only one in the list for that configuration file). Omitted or null
   * means none. Send only when the launcher's `features` include `scenario`.
   */
  scenario?: string | null;
}

/** One entry of GET api/scenarios. */
export interface ScenarioFile {
  /** Path relative to root (send it as is in `scenario` of api/check and api/run). */
  path: string;
  /** File name. */
  name: string;
}

/** GET api/scenarios: the .yaml and .yml files in `scenarios` next to the configuration file. */
export interface ScenarioList {
  /** The configuration file (relative to root). */
  file: string;
  /** The scenario directory (relative to root). */
  directory: string;
  /** Whether the directory exists (a symbolic link to a directory counts as absent). */
  found: boolean;
  scenarios: ScenarioFile[];
  truncated: boolean;
}

export const readScenarioList = (value: unknown): ScenarioList => {
  const path = 'scenarios';
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    directory: field(f, path, 'directory', string),
    found: field(f, path, 'found', boolean),
    scenarios: field(
      f,
      path,
      'scenarios',
      array((item, p) => {
        const g = object(item, p);
        return { path: field(g, p, 'path', string), name: field(g, p, 'name', string) };
      }),
    ),
    truncated: field(f, path, 'truncated', boolean),
  };
};

/**
 * GET api/recording: the values written through the GUI server and applied in the current (or last) run, as scenario
 * YAML. The server writes no file (the screen hands it over as a browser download).
 */
export interface Recording {
  session_id: number;
  /** The run's configuration file (relative to root). */
  file: string;
  state: Open<'running' | 'ended'>;
  /** Whether this run is being recorded (false for a run without variables and for a scenario run). */
  available: boolean;
  /** Why there is no yaml (shown in English, unchanged). */
  reason: string | null;
  /** Number of values put in the YAML. */
  writes: number;
  /** Number of values beyond the limit that were left out. */
  omitted: number;
  /** Number of steps (`set`) in the YAML. */
  steps: number;
  /** Scenario YAML. null while no write has been applied yet. */
  yaml: string | null;
}

/** Answer of GET api/recording (null if there has been no run yet). */
export const readRecording = (value: unknown): Recording | null => {
  const r = field(object(value, 'response'), 'response', 'recording', nullable(object));
  if (r === null) return null;
  const path = 'response.recording';
  return {
    session_id: field(r, path, 'session_id', integer),
    file: field(r, path, 'file', string),
    state: field(r, path, 'state', string),
    available: field(r, path, 'available', boolean),
    reason: field(r, path, 'reason', nullable(string)),
    writes: field(r, path, 'writes', integer),
    omitted: field(r, path, 'omitted', integer),
    steps: field(r, path, 'steps', integer),
    yaml: field(r, path, 'yaml', nullable(string)),
  };
};

/** Takes the launcher's check result out of the answer to POST api/check. */
export const readCheckResponse = (value: unknown): CheckResult =>
  field(object(value, 'response'), 'response', 'check', readCheckResult);

/** Body of POST api/run. */
export interface RunRequest extends CheckRequest {
  speed: Speed;
  start_paused: boolean;
}

/**
 * A participant in the session summary (the participant events laid over each other per name; fields not received yet
 * are null).
 */
export interface SummaryParticipant {
  name: string;
  state: ParticipantState | null;
  pid: number | null;
  exit_code: number | null;
  signal: string | null;
  /** reason of the failed event (why it was counted as failed; kept even if ended arrives later). */
  reason: string | null;
  /** The reason of not_started, or the text of the failed finding. */
  message: string | null;
  /** Names of fields too large to put in the summary (cleared once a later event delivers the field). */
  omitted_fields: string[];
  /** The launcher asked it to stop (a stop_requested event arrived; kept in later states). */
  stop_requested: boolean;
  /** command_line of the manual event (the command that starts it, as one line). null if none. */
  command_line: string | null;
  /** working_directory of the manual event. null if none. */
  working_directory: string | null;
  /** environment of the manual event (environment variables to set when starting it). null if none. */
  environment: Record<string, string> | null;
  /** State of the connection to the ECU's variables (the launcher's control_link event). null until it arrives. */
  control_link: ControlLink | null;
}

const readSummaryParticipant = (
  value: unknown,
  path: string,
  u: Unreadable,
): SummaryParticipant => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    state: field(f, path, 'state', soft(nullable(string), null, u)),
    pid: field(f, path, 'pid', soft(nullable(integer), null, u)),
    exit_code: field(f, path, 'exit_code', soft(nullable(integer), null, u)),
    signal: field(f, path, 'signal', soft(nullable(string), null, u)),
    reason: field(f, path, 'reason', soft(nullable(string), null, u)),
    message: field(f, path, 'message', soft(nullable(string), null, u)),
    omitted_fields: field(f, path, 'omitted_fields', soft(items(string, u), [], u)),
    stop_requested: field(f, path, 'stop_requested', soft(boolean, false, u)),
    command_line: field(f, path, 'command_line', soft(nullable(string), null, u)),
    working_directory: field(f, path, 'working_directory', soft(nullable(string), null, u)),
    environment: field(f, path, 'environment', soft(nullable(record(string)), null, u)),
    control_link: field(
      f,
      path,
      CONTROL_SUMMARY_KEY,
      soft(
        nullable((v, p) => readControlLink(v, p, u)),
        null,
        u,
      ),
    ),
  };
};

/** launcher of the summary: whether the launcher process is running and, if it ended, how. */
export interface SummaryLauncher {
  /**
   * Whether it is running. Stays true after it printed the result, until the process exits. Decides whether it can be
   * stopped.
   */
  running: boolean;
  exit_code: number | null;
  /** Number of the signal that ended it (POSIX only). */
  signal: number | null;
  /**
   * Running but has written nothing for longer than this many seconds (no heartbeat either; it may be stuck). null if
   * not.
   */
  silent_s: number | null;
}

const readSummaryLauncher: Reader<SummaryLauncher> = (value, path) => {
  const f = object(value, path);
  return {
    running: field(f, path, 'running', boolean),
    exit_code: field(f, path, 'exit_code', nullable(integer)),
    signal: field(f, path, 'signal', nullable(integer)),
    silent_s: field(f, path, 'silent_s', optional(nullable(integer), null)),
  };
};

/**
 * The stop the server requested from the launcher. none: not requested; requested: stop in order; forced: stop without
 * waiting.
 */
export type StopRequest = Open<'none' | 'requested' | 'forced'>;

/** Per source, the number of lines the server holds and has dropped. */
export interface LogCounts {
  kept: number;
  dropped: number;
}

const readLogCounts: Reader<LogCounts> = (value, path) => {
  const f = object(value, path);
  return { kept: field(f, path, 'kept', integer), dropped: field(f, path, 'dropped', integer) };
};

function record<T>(read: Reader<T>): Reader<Record<string, T>> {
  return (value, path) =>
    Object.fromEntries(
      Object.entries(object(value, path)).map(([key, item]) => [key, read(item, `${path}.${key}`)]),
    );
}

/**
 * Summary of a server session (one run of the launcher). The screen rebuilds its state from it (the summary is
 * authoritative).
 */
export interface SessionSummary {
  id: number;
  /** Path relative to root. */
  file: string;
  disable: string[];
  /**
   * Participants the user chose on the screen to start themselves (`--manual`; `manual: true` in the configuration file
   * is in the check result).
   */
  manual: string[];
  /** Speed given at the start (null means as in the configuration file). The current speed is time.speed. */
  speed: Speed;
  /** Whether the run started paused. */
  start_paused: boolean;
  /** State as the server sees it. `ended` means the launcher has exited and its output has been read to the end. */
  state: Open<'running' | 'ended'>;
  launcher: SummaryLauncher;
  stop: StopRequest;
  phase: Phase | null;
  last_seq: number | null;
  /** t_ms of the last event received. */
  last_t_ms: number | null;
  /** Virtual time of the last time event. */
  sim_ms: number | null;
  /** Names of the fields of the last time event too large to put in the summary. */
  time_omitted_fields: string[];
  /** The last time event (virtual time, speed, pause, lateness figures). null until it arrives. */
  time: TimeReport | null;
  /** The last command event (whether it was accepted, and why not). null until it arrives. */
  last_command: SummaryCommand | null;
  /**
   * The check result of this run (the check event). null until it arrives. The `session` notifications of a connection
   * that asked for `session_without_check` lack the field and read it as null (the local check result is used as is).
   */
  check: CheckResult | null;
  participants: SummaryParticipant[];
  findings: Finding[];
  result: RunResult | null;
  /** Per source, the number of lines the server holds and has dropped. */
  log: Record<string, LogCounts>;
  problems: ServerProblem[];
  problems_omitted: number;
  /** Progress of the scenario run (`scenario` of `api/run`). null for a run without a scenario. */
  scenario: SummaryScenario | null;
  /**
   * Names of the parts that could not be read (`result.participants`, `findings.code` and so on; set by the screen,
   * not a server field). The missing fields of a result shrunk to fit the limits go here too.
   */
  unreadable: string[];
}

/**
 * `scenario` of the summary: what the server put together from the check, the `scenario_step` events and the result.
 */
export interface SummaryScenario {
  /** The scenario file (relative to root). */
  file: string;
  name: string | null;
  /** Number of steps (from the check result; null until then). */
  step_count: number | null;
  /** Steps received (in index order). After the result, every step (not_run included). */
  steps: ScenarioStep[];
  /** Number of steps beyond the limit that were not kept. */
  steps_omitted: number;
  /** status of the result (null until there is a result). */
  status: ScenarioStatus | null;
  over_at_ms: number | null;
}

const readSummaryScenario = (value: unknown, path: string, u: Unreadable): SummaryScenario => {
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    name: field(f, path, 'name', soft(nullable(string), null, u)),
    step_count: field(f, path, 'step_count', soft(nullable(integer), null, u)),
    steps: field(
      f,
      path,
      'steps',
      items((v, p) => readScenarioStep(v, p, u), u),
    ),
    steps_omitted: field(f, path, 'steps_omitted', soft(integer, 0, u)),
    status: field(f, path, 'status', soft(nullable(string), null, u)),
    over_at_ms: field(f, path, 'over_at_ms', soft(nullable(number), null, u)),
  };
};

/**
 * Reads a session summary part by part. Throws if the core (id, state, launcher, stop) cannot be read. Any other part
 * that cannot be read gets its default value (null, empty) and its name goes into unreadable.
 */
export const readSessionSummary: Reader<SessionSummary> = (value, path) => {
  const f = object(value, path);
  const u = new Unreadable(path);
  const time = field(f, path, 'time', soft(nullable(object), null, u));
  const timePath = `${path}.time`;
  const summary: Omit<SessionSummary, 'unreadable'> = {
    id: field(f, path, 'id', integer),
    state: field(f, path, 'state', string),
    launcher: field(f, path, 'launcher', readSummaryLauncher),
    stop: field(f, path, 'stop', string),
    file: field(f, path, 'file', lenient(string, '', u)),
    disable: field(f, path, 'disable', items(string, u)),
    manual: field(f, path, 'manual', soft(items(string, u), [], u)),
    speed: field(f, path, 'speed', lenient(readSpeed, null, u)),
    start_paused: field(f, path, 'start_paused', soft(boolean, false, u)),
    phase: field(f, path, 'phase', soft(nullable(string), null, u)),
    last_seq: field(f, path, 'last_seq', soft(nullable(integer), null, u)),
    last_t_ms: field(f, path, 'last_t_ms', soft(nullable(number), null, u)),
    sim_ms: time ? field(time, timePath, 'sim_ms', lenient(number, null, u)) : null,
    time_omitted_fields: time
      ? field(time, timePath, 'omitted_fields', soft(items(string, u), [], u))
      : [],
    time: time
      ? lenient((v, p) => readTimeFields(object(v, p), p, u), null, u)(time, timePath)
      : null,
    last_command: field(
      f,
      path,
      'last_command',
      soft(
        nullable((v, p): SummaryCommand => {
          const c = object(v, p);
          return {
            ...readCommandFields(c, p, u),
            seq: field(c, p, 'seq', soft(nullable(integer), null, u)),
          };
        }),
        null,
        u,
      ),
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
    participants: field(
      f,
      path,
      'participants',
      items((v, p) => readSummaryParticipant(v, p, u), u),
    ),
    findings: field(
      f,
      path,
      'findings',
      items((v, p) => readFinding(v, p, u), u),
    ),
    result: field(
      f,
      path,
      'result',
      soft(
        nullable((v, p) => readRunResult(v, p, u)),
        null,
        u,
      ),
    ),
    log: field(f, path, 'log', soft(partlyRecord(readLogCounts, u), {}, u)),
    problems: field(f, path, 'problems', items(readServerProblem, u)),
    problems_omitted: field(f, path, 'problems_omitted', soft(integer, 0, u)),
    scenario: field(
      f,
      path,
      'scenario',
      soft(
        nullable((v, p) => readSummaryScenario(v, p, u)),
        null,
        u,
      ),
    ),
  };
  return { ...summary, unreadable: u.parts };
};

/** Reads the per-source counts, only those that are readable. */
function partlyRecord<T>(read: Reader<T>, u: Unreadable): Reader<Record<string, T>> {
  return (value, path) => {
    const out: Record<string, T> = {};
    for (const [key, item] of Object.entries(object(value, path))) {
      const read1 = lenient(read, undefined, u)(item, `${path}.${key}`);
      if (read1 !== undefined) out[key] = read1;
    }
    return out;
  };
}

/** Answer of GET api/session, POST api/run and POST api/stop (the current session; null if none). */
export const readSessionResponse = (value: unknown): SessionSummary | null =>
  field(object(value, 'response'), 'response', 'session', nullable(readSessionSummary));

/**
 * Body of POST api/stop. force: false stops in order (asking again has no further effect); true stops without waiting.
 */
export interface StopRequestBody {
  force: boolean;
}

/** Names of the time commands (the launcher's stdin commands other than stop). */
export type TimeCommandName = 'pause' | 'resume' | 'step' | 'speed';

/**
 * Body of POST api/time. The server passes it to the launcher as one line and returns at once. Whether it was accepted
 * arrives as a command event, and its effect as a time event.
 */
export type TimeCommand =
  | { command: 'pause' }
  | { command: 'resume' }
  /** Only while paused. Advances by ms and pauses again (the launcher rounds up to a multiple of the time step). */
  | { command: 'step'; ms: number }
  | { command: 'speed'; value: number | 'max' };

/** Answer of POST api/time: the number of the run it was sent to, and the command passed to the launcher. */
export interface TimeAnswer {
  session_id: number;
  command: TimeCommandName;
}

export const readTimeResponse = (value: unknown): TimeAnswer => {
  const path = 'response';
  const f = object(value, path);
  const command = field(f, path, 'command', object);
  return {
    session_id: field(f, path, 'session_id', integer),
    command: field(command, `${path}.command`, 'command', string) as TimeCommandName,
  };
};

// --- GUI server: WebSocket -----------------------------------------------------------------------

/**
 * The first message sent right after connecting. With session and from_seq, the server resends from where that left
 * off.
 */
/** Definition of one variable (`variables.peers[].variables[]` of the summary). */
export interface VariableSummary {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  role: Open<'input' | 'output' | 'parameter'>;
  writable: boolean;
  unit: string;
  min: number | null;
  max: number | null;
  step: number | null;
  description: string;
  /** Items of an enum (when type is enum; null otherwise). */
  enum: { name: string; value: number }[] | null;
}

/** Result of a write (the `operation` notification, and `variables.operations[]` of the summary). */
export interface OperationReport {
  seq: number;
  /** sent, queued (in progress). Once settled: applied, late, rejected, cancelled, invalid, busy, unknown, mixed. */
  state: string;
  done: boolean;
  peers: Record<
    string,
    { state: string; applied_at_ms: number | null; late: boolean; reason: string | null }
  >;
}

/** `variables` of the summary: state of the variable API, variables per participant, recent operations. */
export interface VariablesSummary {
  state: Open<'off' | 'connecting' | 'ready' | 'failed' | 'ended'>;
  reason: string | null;
  peers: { name: string; connected: boolean; variables: VariableSummary[] }[];
  peers_omitted: number;
  variables_omitted: number;
  /** Every unsettled operation (those the server keeps), and the recent operations. */
  operations: OperationReport[];
  /** Every operation numbered at or above this is in operations (null from a server that does not say). */
  operations_from_seq: number | null;
  /** Number of unsettled operations the server could not keep (normally 0). */
  operations_open_omitted: number;
}

const readOperationReport: Reader<OperationReport> = (value, path) => {
  const f = object(value, path);
  return {
    seq: field(f, path, 'seq', integer),
    state: field(f, path, 'state', string),
    done: field(f, path, 'done', boolean),
    peers: field(
      f,
      path,
      'peers',
      record((v, p) => {
        const t = object(v, p);
        return {
          state: field(t, p, 'state', string),
          applied_at_ms: field(t, p, 'applied_at_ms', optional(nullable(number), null)),
          late: field(t, p, 'late', optional(boolean, false)),
          reason: field(t, p, 'reason', optional(nullable(string), null)),
        };
      }),
    ),
  };
};

const readVariableSummary: Reader<VariableSummary> = (value, path) => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', string),
    role: field(f, path, 'role', optional(string, 'output')),
    writable: field(f, path, 'writable', optional(boolean, false)),
    unit: field(f, path, 'unit', optional(string, '')),
    min: field(f, path, 'min', optional(nullable(number), null)),
    max: field(f, path, 'max', optional(nullable(number), null)),
    step: field(f, path, 'step', optional(nullable(number), null)),
    description: field(f, path, 'description', optional(string, '')),
    enum: field(
      f,
      path,
      'enum',
      optional(
        nullable(
          array((v, p) => {
            const item = object(v, p);
            return { name: field(item, p, 'name', string), value: field(item, p, 'value', number) };
          }),
        ),
        null,
      ),
    ),
  };
};

/** Reads `variables` of a summary (present in `hello` and HTTP summaries, absent from `session` notifications). */
export const readVariablesSummary = (value: unknown): VariablesSummary =>
  readVariables(value, 'variables');

const readVariables: Reader<VariablesSummary> = (value, path) => {
  const f = object(value, path);
  return {
    state: field(f, path, 'state', string),
    reason: field(f, path, 'reason', optional(nullable(string), null)),
    peers: field(
      f,
      path,
      'peers',
      optional(
        array((v, p) => {
          const peer = object(v, p);
          return {
            name: field(peer, p, 'name', string),
            connected: field(peer, p, 'connected', optional(boolean, false)),
            variables: field(peer, p, 'variables', optional(array(readVariableSummary), [])),
          };
        }),
        [],
      ),
    ),
    peers_omitted: field(f, path, 'peers_omitted', optional(integer, 0)),
    variables_omitted: field(f, path, 'variables_omitted', optional(integer, 0)),
    operations: field(f, path, 'operations', optional(array(readOperationReport), [])),
    operations_from_seq: field(f, path, 'operations_from_seq', optional(integer, null)),
    operations_open_omitted: field(f, path, 'operations_open_omitted', optional(integer, 0)),
  };
};

/** The `values` notification: the last values of the watched variables, and when they are from. */
export interface ValuesMessage {
  type: 'values';
  /** id of the watch. */
  id: number;
  session: number;
  /** The controller's virtual time (ms; null if unknown). */
  sim_ms: number | null;
  /** true: paused, and the values include every step that began before sim_ms. */
  paused: boolean;
  /**
   * Per participant, the virtual time of the step that read the values, and the values (null for a non-finite real).
   */
  peers: Record<string, { sample_ms: number; values: Record<string, number | boolean | null> }>;
}

/** Answer of `api/write`. */
export interface WriteAnswer {
  session_id: number;
  /** Operation number. The result arrives in `operation` notifications. */
  operation: number;
}

export const readWriteResponse = (value: unknown): WriteAnswer => {
  const f = object(value, 'response');
  return {
    session_id: field(f, 'response', 'session_id', integer),
    operation: field(f, 'response', 'operation', integer),
  };
};

// --- Dashboard files (GET api/dashboard) ---------------------------------------------------------

/**
 * One error in a dashboard file: line and column (from 1), location in the data (`sections[0].items[2].widget`), and
 * English text that says how to fix it.
 */
export interface DashboardFileError {
  line: number;
  column: number;
  at: string;
  message: string;
}

/** A variable written in the file (`<participant>.<variable name>` split up). */
export interface DashboardVariable {
  peer: string;
  name: string;
}

/**
 * An item in the file, of exactly one of these shapes (the parser drops items of an unknown shape).
 * - variable: shows a variable with a widget (the default widget if widget is null).
 * - panel: a custom panel (the name of a custom element).
 * - auto: the variables of that participant not placed elsewhere in the file (with the default widgets).
 */
export type DashboardFileItem =
  | {
      variable: DashboardVariable;
      widget: string | null;
      label: string | null;
      options: Record<string, unknown>;
    }
  | {
      panel: string;
      label: string | null;
      options: Record<string, unknown>;
      variables: DashboardVariable[];
    }
  | { auto: string };

/** A section of the file. */
export interface DashboardFileSection {
  title: string;
  collapsed: boolean;
  items: DashboardFileItem[];
}

/** A custom panel module: the name written in the file, and the URL to load (relative to the screen's base). */
export interface DashboardPanelModule {
  module: string;
  url: string;
}

/** Contents of a dashboard file, as checked by the server. */
export interface DashboardFile {
  title: string | null;
  panels: DashboardPanelModule[];
  sections: DashboardFileSection[];
}

/**
 * `GET api/dashboard?file=<configuration file>`: `<name>.dashboard.yaml` next to the configuration file. When there is
 * none (found is false) or it has errors (errors; dashboard is null), the screen uses the automatic dashboard.
 */
export interface DashboardResponse {
  /** The configuration file (relative to root). */
  file: string;
  /** The dashboard file (relative to root). */
  path: string;
  found: boolean;
  errors: DashboardFileError[];
  dashboard: DashboardFile | null;
}

const readDashboardVariable: Reader<DashboardVariable> = (value, path) => {
  const f = object(value, path);
  return { peer: field(f, path, 'peer', string), name: field(f, path, 'name', string) };
};

const readOptions: Reader<Record<string, unknown>> = (value, path) => ({ ...object(value, path) });

const readDashboardItem = (value: unknown, path: string): DashboardFileItem | null => {
  const f = object(value, path);
  const label = (): string | null => field(f, path, 'label', nullable(string));
  if (f['variable'] !== undefined) {
    return {
      variable: field(f, path, 'variable', readDashboardVariable),
      widget: field(f, path, 'widget', nullable(string)),
      label: label(),
      options: field(f, path, 'options', readOptions),
    };
  }
  if (f['panel'] !== undefined) {
    return {
      panel: field(f, path, 'panel', string),
      label: label(),
      options: field(f, path, 'options', readOptions),
      variables: field(f, path, 'variables', array(readDashboardVariable)),
    };
  }
  if (f['auto'] !== undefined) return { auto: field(f, path, 'auto', string) };
  return null; // An item of an unknown shape
};

const readDashboardFile: Reader<DashboardFile> = (value, path) => {
  const f = object(value, path);
  return {
    title: field(f, path, 'title', nullable(string)),
    panels: field(
      f,
      path,
      'panels',
      array((item, p) => {
        const m = object(item, p);
        return { module: field(m, p, 'module', string), url: field(m, p, 'url', string) };
      }),
    ),
    sections: field(
      f,
      path,
      'sections',
      array((item, p) => {
        const s = object(item, p);
        return {
          title: field(s, p, 'title', string),
          collapsed: field(s, p, 'collapsed', boolean),
          items: field(s, p, 'items', array(readDashboardItem)).filter(
            (entry): entry is DashboardFileItem => entry !== null,
          ),
        };
      }),
    ),
  };
};

export const readDashboardResponse = (value: unknown): DashboardResponse => {
  const path = 'response';
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    path: field(f, path, 'path', string),
    found: field(f, path, 'found', boolean),
    errors: field(
      f,
      path,
      'errors',
      array((item, p) => {
        const e = object(item, p);
        return {
          line: field(e, p, 'line', integer),
          column: field(e, p, 'column', integer),
          at: field(e, p, 'at', string),
          message: field(e, p, 'message', string),
        };
      }),
    ),
    dashboard: field(f, path, 'dashboard', nullable(readDashboardFile)),
  };
};

/**
 * Notifications the screen sends after authentication: start a variable watch (replacing one with the same id), or stop
 * it.
 */
export type WatchMessage =
  | {
      type: 'watch';
      id: number;
      variables: 'all' | { peer: string; name: string }[];
      period_ms?: number;
    }
  | { type: 'unwatch'; id: number };

export interface AuthMessage {
  type: 'auth';
  token: string;
  session?: number;
  from_seq?: number;
  /**
   * If true, the summaries in this connection's `session` notifications leave out `check` (it arrives in the `hello`
   * summary and the `check` event), because the check result is large and does not change during the run.
   */
  session_without_check?: boolean;
}

/** replay of hello: where the resend starts (null: from the beginning). */
export interface Replay {
  from_seq: number | null;
}

const readReplay: Reader<Replay> = (value, path) => {
  const f = object(value, path);
  return { from_seq: field(f, path, 'from_seq', nullable(integer)) };
};

/** dropped: how much was not sent to this screen. */
export interface Dropped {
  type: 'dropped';
  session: number | null;
  /**
   * not-kept: the server no longer holds it (before the resend); slow-client: the screen read too slowly and the server
   * dropped some of what it was to send.
   */
  reason: Open<'not-kept' | 'slow-client'>;
  /** Per source, the number of log lines dropped. */
  log: Record<string, number>;
  /** Sources whose count is only a lower bound (more may have been dropped). */
  at_least: string[];
  /** Number of other events dropped. */
  events: number;
}

export type ServerMessage =
  /**
   * Right after connecting (and after resync): summary of the current session, and a description of the resend that
   * follows.
   */
  | { type: 'hello'; instance: string; session: SessionSummary | null; replay: Replay | null }
  /** A launcher event (resent log lines as well as new events). */
  | { type: 'event'; session: number; event: LauncherEvent }
  /** The resend has finished. */
  | { type: 'synced'; session: number | null; last_seq: number | null }
  /** The session summary changed (started, stop requested, the launcher exited, the run ended). */
  | { type: 'session'; session: SessionSummary }
  /**
   * A server notice (a gap in seq, an invalid line from the launcher, an end without a result, failed authentication
   * and so on).
   */
  | ({ type: 'error'; session: number | null } & ServerProblem)
  /** How much was not sent. */
  | Dropped
  /** The screen read too slowly and messages were dropped. Everything is sent again, starting with hello. */
  | { type: 'resync'; message: string }
  /** The state or the list of the variable API changed (also once right after a run starts). */
  | { type: 'variables'; session: number; variables: VariablesSummary }
  /** Values of the watched variables. */
  | ValuesMessage
  /** The result of a write changed. */
  | ({ type: 'operation'; session: number } & OperationReport);

/**
 * Reads one WebSocket notification. Unknown notifications, and notifications wrapping an unknown event, are null
 * (skipped).
 */
export const readServerMessage = (value: unknown): ServerMessage | null => {
  const path = 'message';
  const f = object(value, path);
  const type = field(f, path, 'type', string);
  switch (type) {
    case 'hello':
      version(f, path, 'vehicle_sil_gui', PROTOCOL_VERSION);
      return {
        type,
        instance: field(f, path, 'instance', string),
        session: field(f, path, 'session', nullable(readSessionSummary)),
        replay: field(f, path, 'replay', nullable(readReplay)),
      };
    case 'event': {
      const event = field(f, path, 'event', (v, p) => readLauncherEvent(v, p, true));
      return event && { type, session: field(f, path, 'session', integer), event };
    }
    case 'synced':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        last_seq: field(f, path, 'last_seq', nullable(integer)),
      };
    case 'session':
      return { type, session: field(f, path, 'session', readSessionSummary) };
    case 'error':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        ...readServerProblem(value, path),
      };
    case 'dropped':
      return {
        type,
        session: field(f, path, 'session', nullable(integer)),
        reason: field(f, path, 'reason', string),
        log: field(f, path, 'log', record(integer)),
        at_least: field(f, path, 'at_least', array(string)),
        events: field(f, path, 'events', integer),
      };
    case 'resync':
      return { type, message: field(f, path, 'message', optional(string, '')) };
    case 'variables':
      return {
        type,
        session: field(f, path, 'session', integer),
        variables: field(f, path, 'variables', readVariables),
      };
    case 'values':
      return {
        type,
        id: field(f, path, 'id', integer),
        session: field(f, path, 'session', integer),
        sim_ms: field(f, path, 'sim_ms', nullable(number)),
        paused: field(f, path, 'paused', boolean),
        peers: field(
          f,
          path,
          'peers',
          record((v, p) => {
            const peer = object(v, p);
            return {
              sample_ms: field(peer, p, 'sample_ms', number),
              values: field(
                peer,
                p,
                'values',
                record((x, xp) => {
                  if (x !== null && typeof x !== 'boolean') return number(x, xp);
                  return x;
                }),
              ),
            };
          }),
        ),
      };
    case 'operation':
      return {
        type,
        session: field(f, path, 'session', integer),
        ...readOperationReport(value, path),
      };
    default:
      return null;
  }
};
