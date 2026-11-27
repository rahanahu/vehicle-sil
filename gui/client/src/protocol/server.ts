/**
 * The shapes the GUI server decides: the HTTP API (`api/*`), the WebSocket messages and the dashboard files. Parts that
 * come from the launcher (check, result, events) have the shapes of launcher.ts. Rules of the parsers: read.ts.
 */

import {
  Open,
  ProtocolError,
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
} from './read';
import {
  CONTROL_SUMMARY_KEY,
  CheckResult,
  ControlLink,
  Finding,
  LauncherEvent,
  ParticipantState,
  Phase,
  RunResult,
  ScenarioStatus,
  ScenarioStep,
  Speed,
  SummaryCommand,
  SummaryState,
  TimeReport,
  Caution,
  readCaution,
  readCheckResult,
  readCommandFields,
  readControlLink,
  readFinding,
  readLauncherEvent,
  readRunResult,
  readScenarioStep,
  readStateFields,
  readSpeed,
  readTimeFields,
} from './launcher';

/**
 * The version of the GUI server's API that this library reads (`vehicle_sil_gui` of `api/app` and `hello`). It does not
 * read a hello or an api/app of another version (their fields may mean something else). When to raise the version:
 * "Versioning and compatibility" in gui/protocol/README.md.
 */
export const PROTOCOL_VERSION = 4;

// --- GUI server: common --------------------------------------------------------------------------

const nonEmptyString: Reader<string> = (value, path) => {
  if (typeof value !== 'string' || !value) throw new ProtocolError(path, 'a non-empty string');
  return value;
};

/**
 * A server notice (an API error, a WebSocket error, problems of the session). message is shown in English, unchanged.
 */
export interface ServerProblem {
  code: string;
  message: string;
  /** For launcher-stderr, the number of lines dropped at this point (`detail.skipped`). Other notices lack it. */
  skipped?: number;
  /** For `check-failed`, the findings of the check that refused the start (`detail.findings`; unreadable ones left out). */
  findings?: Finding[];
  /** For `write-timeout` and `write-failed` after an operation number was assigned (`detail.operation`). */
  operation?: number;
  /** Last known operation state on `write-timeout` / `write-failed` (`detail.state`, e.g. `'sent'`). */
  state?: string;
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
  if (typeof detail === 'object' && detail !== null && 'findings' in detail) {
    const list = detail.findings;
    if (Array.isArray(list)) {
      const findings: Finding[] = [];
      list.forEach((item, i) => {
        try {
          findings.push(readFinding(item, `${path}.detail.findings[${i}]`));
        } catch {
          // A finding that cannot be read is left out (the message already says what failed)
        }
      });
      problem.findings = findings;
    }
  }
  if (typeof detail === 'object' && detail !== null && 'operation' in detail) {
    const op = detail.operation;
    if (typeof op === 'number' && Number.isSafeInteger(op) && op >= 0) {
      problem.operation = op;
    }
  }
  if (typeof detail === 'object' && detail !== null && 'state' in detail) {
    const st = detail.state;
    if (typeof st === 'string' && st.length > 0) {
      problem.state = st;
    }
  }
  return problem;
};

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
  /** The server recognized a simulation configuration. Older servers may omit this flag. */
  simulation?: boolean;
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
      array((item, p) => {
        const entry = object(item, p);
        const simulation = field(entry, p, 'simulation', optional(boolean, undefined));
        return {
          path: field(entry, p, 'path', string),
          ...(simulation === undefined ? {} : { simulation }),
        };
      }),
    ),
    truncated: field(f, path, 'truncated', boolean),
  };
};

/** What the launcher does once a scenario passes. */
export type AfterScenario = 'stop' | 'pause';

/**
 * Body of POST api/check: `file` (this server's field) and the keys of the request file that the launcher reads
 * (`schema/run-request.schema.json`), with the same names and value forms. A key that is not wanted is left out: there
 * is no null.
 */
export interface CheckRequest {
  /** Path relative to root. */
  file: string;
  /** The format of the request file: 1, or left out. */
  vehicle_sil_run_request?: 1;
  /** Names of the participants to leave out (only names in the check result). */
  disable: string[];
  /** Names of the participants to take in although the configuration file disables them. */
  enable?: string[];
  /** Names of the participants the user starts (only names in the check result). */
  manual?: string[];
  /** The scenario (`path` from `GET api/scenarios`; only one in the list for that configuration file). */
  scenario?: string;
  /** What to do after the scenario passes (`stop`, or `pause` to hand over to manual control). */
  after_scenario?: AfterScenario;
  /** The variant of the configuration file. */
  variant?: string;
  /** Values of the scenario's parameters (name to value); used only when a scenario runs. */
  params?: Record<string, string | number | boolean>;
  /** true: fail on warnings too. */
  strict?: boolean;
  /** Exact virtual time at which to stop. Omit for no automatic stop. */
  stop_at_ms?: number;
  /** Wall-clock milliseconds of lateness before a paced step counts as late. */
  late_tolerance_ms?: number;
  /**
   * Where networks are connected outside the simulation: network name to target. For a network of the file, a target
   * other than `none` must be a `value` that the last `POST api/connections` of that network gave with `usable: true`
   * (else 400 target-not-listed); a network that the file lacks is left out by the launcher, with a note.
   */
  connect?: Record<string, string>;
  /** The time policy of a run connected outside (a name that the launcher takes, e.g. `free`; it decides what it allows). */
  time_policy?: string;
}

/**
 * Body of POST api/check with `text`: the text of `file` that is not saved yet. The check takes only the
 * configuration (`variant`, `strict`), not the choices of a run, so there is no `disable` or `scenario` here.
 */
export interface CheckTextRequest {
  /** Path relative to root: the file whose content `text` is (it must exist; its directory is where paths start). */
  file: string;
  /** The format of the request file: 1, or left out. */
  vehicle_sil_run_request?: 1;
  /** The content of the file as the screen has it (at most 1 MiB as UTF-8). */
  text: string;
  /** The variant of the configuration file. */
  variant?: string;
  /** true: fail on warnings too. */
  strict?: boolean;
}

/** Body of POST api/describe: the saved file and the variant whose participants are described again. */
export interface DescribeRequest {
  /** Path relative to root. */
  file: string;
  /** The variant of the configuration file (left out: none). */
  variant?: string;
}

/** Body of POST api/connections: the configuration file and one of its networks (one the user opened). */
export interface ConnectionsRequest {
  /** Path relative to root. */
  file: string;
  /** A name in `networks[].name` of the last check of `file`. */
  network: string;
}

/** Answer of POST api/connections: where that network can be connected on this computer. */
export interface Connections {
  file: string;
  network: string;
  /** The exit code of the launcher's `connections` (0: listed, 1: a list failed or the file has errors, 4: usage). */
  exit_code: number;
  /**
   * The launcher's `connections --json` as it wrote it (`networks[].list.targets[]`: `value`, `label`, `detail`,
   * `usable`, `reason`). A `value` with `usable: true` is what `connect` of api/check and api/run may give the network.
   */
  connections: Record<string, unknown>;
}

/** Reads the answer of POST api/connections. */
export const readConnections = (value: unknown): Connections => {
  const path = 'connections';
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    network: field(f, path, 'network', string),
    exit_code: field(f, path, 'exit_code', integer),
    connections: field(f, path, 'connections', object),
  };
};

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
  run_id: string;
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
  /** Finalized end virtual time (ms) of the run once ended, or null while running / unavailable. */
  end_ms: number | null;
  /** Scenario YAML. null while no write has been applied yet. */
  yaml: string | null;
}

/** Answer of GET api/recording (null if there has been no run yet). */
export const readRecording = (value: unknown): Recording | null => {
  const r = field(object(value, 'response'), 'response', 'recording', nullable(object));
  if (r === null) return null;
  const path = 'response.recording';
  return {
    run_id: field(r, path, 'run_id', nonEmptyString),
    file: field(r, path, 'file', string),
    state: field(r, path, 'state', string),
    available: field(r, path, 'available', boolean),
    reason: field(r, path, 'reason', nullable(string)),
    writes: field(r, path, 'writes', nonNegativeInteger),
    omitted: field(r, path, 'omitted', nonNegativeInteger),
    steps: field(r, path, 'steps', nonNegativeInteger),
    end_ms: field(r, path, 'end_ms', optional(nullable(nonNegativeInteger), null)),
    yaml: field(r, path, 'yaml', nullable(string)),
  };
};

/** Answer of GET api/config: the text of a configuration file as it is saved. */
export interface ConfigText {
  /** Path relative to root. */
  file: string;
  /** The file's bytes read as UTF-8, as they are (a BOM and the line ends too). */
  text: string;
  /** SHA-256 of the file's bytes (64 lowercase hexadecimal digits). */
  stamp: string;
}

/** Reads the answer of GET api/config. */
export const readConfigText = (value: unknown): ConfigText => {
  const path = 'config';
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    text: field(f, path, 'text', string),
    stamp: field(f, path, 'stamp', string),
  };
};

/**
 * Body of POST api/config: saves `text` as `file`. By default the server saves only an edit whose commands that run
 * stay the same (the launcher's `check --compare-commands` says `"same"`); `allowed.save` says what applies.
 */
export interface SaveConfigRequest {
  /** Path relative to root. */
  file: string;
  /** The whole content of the file (at most 1 MiB as UTF-8). Written as it is: a BOM and the line ends stay. */
  text: string;
  /** The `stamp` of GET api/config for the file as it was read, or null for a new file. */
  stamp: string | null;
}

/** Answer of POST api/config: the file that was saved and the stamp of its new content. */
export interface SavedConfig {
  /** Path relative to root. */
  file: string;
  /** SHA-256 of the bytes that were saved (the stamp to send with the next save). */
  stamp: string;
}

/** Reads the answer of POST api/config. */
export const readSavedConfig = (value: unknown): SavedConfig => {
  const path = 'saved';
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    stamp: field(f, path, 'stamp', string),
  };
};

/** Takes the launcher's check result out of the answer to POST api/check (and POST api/describe). */
export const readCheckResponse = (value: unknown): CheckResult =>
  field(object(value, 'response'), 'response', 'check', readCheckResult);

/** Body of POST api/run. */
export interface RunRequest extends CheckRequest {
  /** Simulation seconds per wall-clock second, or `max`; left out: as the configuration file says. */
  speed?: number | 'max';
  /** true: stand still at 0 until a resume or step; left out: no. */
  start_paused?: boolean;
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

export type LauncherMode = Open<'cold' | 'hot'>;

export type RuntimeMode = Open<'cold' | 'hot'>;

export type RuntimeState = Open<'idle' | 'preparing' | 'ready' | 'running' | 'resetting' | 'cold'>;

/** Persistent launcher status reported on `api/session`, `api/prepare`, `hello`, and `runtime` notifications. */
export interface RuntimeStatus {
  mode: RuntimeMode;
  state: RuntimeState;
  config: string | null;
  reason: string | null;
  message: string | null;
}

export const readRuntimeStatus: Reader<RuntimeStatus> = (value, path) => {
  const f = object(value, path);
  return {
    mode: field(f, path, 'mode', string),
    state: field(f, path, 'state', string),
    config: field(f, path, 'config', nullable(string)),
    reason: field(f, path, 'reason', nullable(string)),
    message: field(f, path, 'message', nullable(string)),
  };
};

export interface PrepareRequest {
  file: string;
  variant?: string;
  disable?: string[];
  enable?: string[];
  manual?: string[];
}

export interface PrepareResponse {
  instance: string;
  runtime?: RuntimeStatus;
}

export const readPrepareResponse = (value: unknown): PrepareResponse => {
  const path = 'response';
  const f = object(value, path);
  const runtime = field(f, path, 'runtime', optional(readRuntimeStatus, undefined));
  return {
    instance: field(f, path, 'instance', nonEmptyString),
    ...(runtime === undefined ? {} : { runtime }),
  };
};

/** launcher of the summary: whether the launcher process is running and, if it ended, how. */
export interface SummaryLauncher {
  /**
   * Whether the run is still active on its launcher (false once ended, even when a persistent launcher stays alive).
   */
  running: boolean;
  /** Persistent `'hot'` or per-run `'cold'`. Omitted on older servers. */
  mode?: LauncherMode;
  /** Whether the underlying launcher process itself is still alive. Omitted on older servers. */
  process_alive?: boolean;
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
  const mode = field(f, path, 'mode', optional(string, undefined));
  const processAlive = field(f, path, 'process_alive', optional(boolean, undefined));
  return {
    running: field(f, path, 'running', boolean),
    ...(mode === undefined ? {} : { mode }),
    ...(processAlive === undefined ? {} : { process_alive: processAlive }),
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

/**
 * Summary of a server session (one run of the launcher). The screen rebuilds its state from it (the summary is
 * authoritative).
 */
export interface SessionSummary {
  id: string;
  /** Baseline launcher t_ms at run start (0 on cold runs). */
  run_t0_ms?: number | null;
  /** Path relative to root. */
  file: string;
  disable: string[];
  /**
   * Participants the user chose on the screen to start themselves (`--manual`; `manual: true` in the configuration file
   * is in the check result).
   */
  manual: string[];
  /** Speed requested at the start (null means no speed was requested). The current speed is `speed` of launcher_state. */
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
  /** The last time event (virtual time, actual speed, lateness figures; whether it is paused, where, and the speed set are in launcher_state). null until it arrives. */
  time: TimeReport | null;
  /** The last command event (whether it was accepted, and why not). null until it arrives. */
  last_command: SummaryCommand | null;
  /**
   * The last state event of the launcher (what it accepts now); only the last is kept. null until it arrives. The
   * parsers always set it; it may be absent in a summary built by hand.
   */
  launcher_state?: SummaryState | null;
  /**
   * What this server accepts now (start, stop, force). null from a server that does not say. The parsers always set
   * it; it may be absent in a summary built by hand.
   */
  allowed?: ServerAllowed | null;
  /**
   * The check result of this run (the check event). null until it arrives.
   */
  check: CheckResult | null;
  /** The checked stop time when the full check was too large to keep. */
  stop_at_ms?: number | null;
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
  /** Selected case name, or null when none was selected. */
  case?: string | null;
  /** Number of steps (from the check result; null until then). */
  step_count: number | null;
  /** Steps received (in index order). After the result, every step (not_run included). */
  steps: ScenarioStep[];
  /** Number of steps beyond the limit that were not kept. */
  steps_omitted: number;
  /** status of the result (null until there is a result). */
  status: ScenarioStatus | null;
  over_at_ms: number | null;
  /** Whether the scenario passed and handed over to manual control. */
  handed_over?: boolean;
  /**
   * Where the server serves the run's timeline file (`api/timeline/<session id>`, relative to the server's URL; needs
   * the token): set at the end of a run whose launcher wrote one, else null.
   */
  timeline_url: string | null;
}

const readSummaryScenario = (value: unknown, path: string, u: Unreadable): SummaryScenario => {
  const f = object(value, path);
  return {
    file: field(f, path, 'file', string),
    name: field(f, path, 'name', soft(nullable(string), null, u)),
    ...(f['case'] === undefined
      ? {}
      : { case: field(f, path, 'case', soft(nullable(string), null, u)) }),
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
    ...(f['handed_over'] === undefined
      ? {}
      : { handed_over: field(f, path, 'handed_over', soft(boolean, false, u)) }),
    timeline_url: field(f, path, 'timeline_url', soft(nullable(string), null, u)),
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
  const stopAt = field(f, path, 'stop_at_ms', soft(nullable(integer), undefined, u));
  const summary: Omit<SessionSummary, 'unreadable'> = {
    id: field(f, path, 'id', nonEmptyString),
    ...(f['run_t0_ms'] === undefined
      ? {}
      : { run_t0_ms: field(f, path, 'run_t0_ms', soft(nullable(number), null, u)) }),
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
    launcher_state: field(
      f,
      path,
      'launcher_state',
      soft(
        nullable((v, p): SummaryState => {
          const s = object(v, p);
          return {
            ...readStateFields(s, p, u),
            seq: field(s, p, 'seq', soft(nullable(integer), null, u)),
          };
        }),
        null,
        u,
      ),
    ),
    allowed: field(f, path, 'allowed', soft(nullable(readServerAllowed), null, u)),
    ...(stopAt === undefined ? {} : { stop_at_ms: stopAt }),
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

/**
 * Whether this server takes one request now. `ok` says it would; `reason` is a code that stays the same (`null` if
 * ok), `message` one English sentence (`null` if ok).
 */
export interface ServerAllowedItem {
  ok: boolean;
  reason: string | null;
  message: string | null;
}

/** `save` of `allowed`: also `caution`, `{code, message}` when the server saves but there is something to know first. */
export interface ServerSaveItem extends ServerAllowedItem {
  /** `command-changes-allowed`: the server also saves edits that change the commands that run, and new files. */
  caution: Caution | null;
}

/**
 * What this server accepts now (`allowed` of the summary, of `hello` and of `api/session`). start: reasons
 * shutting-down, starting, already-running, launcher-unusable. stop and force: shutting-down, not-running,
 * already-stopping (stop), already-forced (force). save: shutting-down, save-disabled. What the launcher accepts is
 * `launcher_state.allowed`.
 */
export interface ServerAllowed {
  start: ServerAllowedItem;
  stop: ServerAllowedItem;
  force: ServerAllowedItem;
  /** POST api/config. */
  save: ServerSaveItem;
}

const readServerAllowedItem: Reader<ServerAllowedItem> = (value, path) => {
  const f = object(value, path);
  return {
    ok: field(f, path, 'ok', boolean),
    reason: field(f, path, 'reason', optional(nullable(string), null)),
    message: field(f, path, 'message', optional(nullable(string), null)),
  };
};

const readServerSaveItem: Reader<ServerSaveItem> = (value, path) => ({
  ...readServerAllowedItem(value, path),
  caution: field(object(value, path), path, 'caution', nullable(readCaution)),
});

export const readServerAllowed: Reader<ServerAllowed> = (value, path) => {
  const f = object(value, path);
  return {
    start: field(f, path, 'start', readServerAllowedItem),
    stop: field(f, path, 'stop', readServerAllowedItem),
    force: field(f, path, 'force', readServerAllowedItem),
    save: field(f, path, 'save', readServerSaveItem),
  };
};

/** Reads the per-source counts, only those that are readable. */
function partlyRecord<T>(read: Reader<T>, u?: Unreadable): Reader<Record<string, T>> {
  if (!u) return record(read);
  return (value, path) => {
    const out: Record<string, T> = {};
    for (const [key, item] of Object.entries(object(value, path))) {
      const read1 = lenient(read, undefined, u)(item, `${path}.${key}`);
      if (read1 !== undefined) out[key] = read1;
    }
    return out;
  };
}

/** Answer of GET api/session (the current session; null if none). */
export const readSessionResponse = (value: unknown): SessionSummary | null =>
  field(object(value, 'response'), 'response', 'session', nullable(readSessionSummary));

/** GET api/session, POST api/run and POST api/stop-session must identify the GUI server that supplied the run ID. */
export const readSessionInstance = (value: unknown): string =>
  field(object(value, 'response'), 'response', 'instance', (instance, path) => {
    if (typeof instance !== 'string' || !instance)
      throw new ProtocolError(path, 'a server instance ID. Use the GUI server from this release');
    return instance;
  });

/** Answer of POST api/run and POST api/stop-session. */
export interface RunAck {
  instance: string;
  run_id: string;
}

export const readRunResponse = (value: unknown): RunAck => {
  const path = 'response';
  const f = object(value, path);
  return {
    instance: readSessionInstance(value),
    run_id: field(f, path, 'run_id', nonEmptyString),
  };
};

/** What the server accepts, from `allowed` of an `api/session` response: also there when there is no run. */
export const readSessionAllowed = (value: unknown): ServerAllowed | null =>
  field(
    object(value, 'response'),
    'response',
    'allowed',
    optional(nullable(readServerAllowed), null),
  );

/** Persistent runtime status carried alongside `api/session` (undefined on older servers). */
export const readSessionRuntime = (value: unknown): RuntimeStatus | null | undefined =>
  field(
    object(value, 'response'),
    'response',
    'runtime',
    optional(nullable(readRuntimeStatus), undefined),
  );

/**
 * POST api/stop-session: the server stops only this session. force: false stops in order (asking again has no further
 * effect); true stops without waiting.
 */
export interface StopSessionRequestBody {
  force: boolean;
  run_id: string;
  instance: string;
}

/** Names of the time commands (the launcher's stdin commands other than stop). */
export type TimeCommandName = 'pause' | 'resume' | 'step' | 'run_until' | 'speed';

/**
 * The command sent in POST api/time. The GUI Client adds the run's run_id and server instance to the request.
 * The server passes the command to the launcher as one line. The answer arrives as a command event.
 */
export type TimeCommand = (
  | { command: 'pause' }
  | { command: 'resume' }
  /** Only while paused. Advances by ms and pauses again (the launcher rounds up to a multiple of the time step). */
  | { command: 'step'; ms: number }
  /** Runs on and pauses at at_ms (0 to limits.step_ms_max; the launcher rounds up to a multiple of the time step). */
  | { command: 'run_until'; at_ms: number }
  | { command: 'speed'; value: number | 'max' }
) & {
  /**
   * A whole number from 0 to 2^53 of the sender's choice. The launcher returns it as `id` of the command event that
   * answers this command, so the sender tells that answer from the answers to other commands. Needs a launcher with
   * the feature `state`.
   */
  id?: number;
};

/** Answer of POST api/time: the run_id of the run it was sent to, and the command passed to the launcher. */
export interface TimeAnswer {
  run_id: string;
  command: TimeCommandName;
  /** The `id` the request had (null if none). */
  id: number | null;
}

export const readTimeResponse = (value: unknown): TimeAnswer => {
  const path = 'response';
  const f = object(value, path);
  const command = field(f, path, 'command', object);
  return {
    run_id: field(f, path, 'run_id', nonEmptyString),
    command: field(command, `${path}.command`, 'command', string) as TimeCommandName,
    id: field(command, `${path}.command`, 'id', optional(nullable(integer), null)),
  };
};

// --- GUI server: WebSocket -----------------------------------------------------------------------

/**
 * The first message sent right after connecting. With session and from_seq, the server resends from where that left
 * off.
 */
/**
 * Whether the ECU uses a variable now (information only; it never changes how the variable is read or written).
 * unknown: the ECU has not said yet; active: in use (also every variable that does not report it); inactive: not used
 * by the ECU now.
 */
export type VariableAvailability = 'unknown' | 'active' | 'inactive';

/** Reads an availability. A value this library does not know, or none (an older server), is `active`. */
export const readAvailability = (value: unknown): VariableAvailability =>
  value === 'unknown' || value === 'inactive' ? value : 'active';

/** Definition of one variable (`variables.peers[].variables[]` of the summary). */
export interface VariableSummary {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  role: Open<'input' | 'output' | 'parameter'>;
  writable: boolean;
  /** Whether vehicle-sil added the variable (`ecu.connected`, `peer.*`, `j1939.*`, `can.*`, `lin.*`, `eth.*`) rather than the ECU application declaring it. */
  builtin: boolean;
  unit: string;
  min: number | null;
  max: number | null;
  step: number | null;
  description: string;
  /** Items of an enum (when type is enum; null otherwise). */
  enum: { name: string; value: number }[] | null;
  /** Whether the ECU uses it now (`active` from a server that does not say). */
  availability: VariableAvailability;
}

/** Result of a write (the `operation` notification, and `variables.operations[]` of the summary). */
export interface OperationReport {
  seq: number;
  /** sent, queued (in progress). Once settled: applied, late, rejected, cancelled, invalid, busy, unknown, mixed. */
  state: string;
  done: boolean;
  peers: Record<
    string,
    {
      state: string;
      applied_at_ms: number | null;
      late: boolean;
      reason: string | null;
      /** How many of the participant's writes took effect, from the first (null: not known). */
      applied_writes: number | null;
      /** Position (from 0) of the write `reason` is about (null: none, or not known). */
      failed_write: number | null;
    }
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

const safeInteger: Reader<number> = (value, path) => {
  if (!Number.isSafeInteger(value)) throw new ProtocolError(path, 'an integer');
  return value as number;
};

const nonNegativeInteger: Reader<number> = (value, path) => {
  const n = safeInteger(value, path);
  if (n < 0) throw new ProtocolError(path, 'a non-negative integer');
  return n;
};

export const readOperationReport = (
  value: unknown,
  path: string,
  u?: Unreadable,
): OperationReport => {
  const f = object(value, path);
  return {
    seq: field(f, path, 'seq', safeInteger),
    state: field(f, path, 'state', string),
    done: field(f, path, 'done', lenient(boolean, false, u)),
    peers: field(
      f,
      path,
      'peers',
      soft(
        partlyRecord((v, p) => {
          const t = object(v, p);
          return {
            state: field(t, p, 'state', lenient(string, 'unknown', u)),
            applied_at_ms: field(t, p, 'applied_at_ms', soft(nullable(number), null, u)),
            late: field(t, p, 'late', soft(boolean, false, u)),
            reason: field(t, p, 'reason', soft(nullable(string), null, u)),
            applied_writes: field(
              t,
              p,
              'applied_writes',
              soft(nullable(nonNegativeInteger), null, u),
            ),
            failed_write: field(t, p, 'failed_write', soft(nullable(nonNegativeInteger), null, u)),
          };
        }, u),
        {},
        u,
      ),
    ),
  };
};

const readEnumItem: Reader<{ name: string; value: number }> = (value, path) => {
  const item = object(value, path);
  return { name: field(item, path, 'name', string), value: field(item, path, 'value', number) };
};

const readVariableSummary = (value: unknown, path: string, u?: Unreadable): VariableSummary => {
  const f = object(value, path);
  return {
    name: field(f, path, 'name', string),
    type: field(f, path, 'type', lenient(string, '', u)),
    role: field(f, path, 'role', soft(string, 'output', u)),
    writable: field(f, path, 'writable', soft(boolean, false, u)),
    builtin: field(f, path, 'builtin', soft(boolean, false, u)),
    unit: field(f, path, 'unit', soft(string, '', u)),
    min: field(f, path, 'min', soft(nullable(number), null, u)),
    max: field(f, path, 'max', soft(nullable(number), null, u)),
    step: field(f, path, 'step', soft(nullable(number), null, u)),
    description: field(f, path, 'description', soft(string, '', u)),
    enum: field(
      f,
      path,
      'enum',
      soft(
        nullable((v, p) => {
          if (!Array.isArray(v)) throw new ProtocolError(p, 'an array');
          return items(readEnumItem, u)(v, p);
        }),
        null,
        u,
      ),
    ),
    availability: readAvailability(f['availability']),
  };
};

/** Reads `variables` of a summary (present in `hello` and HTTP summaries, absent from `session` notifications). */
export const readVariablesSummary = (value: unknown): VariablesSummary =>
  readVariablesReport(value, 'variables');

export const readVariablesReport = (
  value: unknown,
  path: string,
  u?: Unreadable,
): VariablesSummary => {
  const f = object(value, path);
  return {
    state: field(f, path, 'state', lenient(string, 'unknown', u)),
    reason: field(f, path, 'reason', soft(nullable(string), null, u)),
    peers: field(
      f,
      path,
      'peers',
      soft(
        items((v, p) => {
          const peer = object(v, p);
          return {
            name: field(peer, p, 'name', string),
            connected: field(peer, p, 'connected', soft(boolean, false, u)),
            variables: field(
              peer,
              p,
              'variables',
              soft(
                items((item, ip) => readVariableSummary(item, ip, u), u),
                [],
                u,
              ),
            ),
          };
        }, u),
        [],
        u,
      ),
    ),
    peers_omitted: field(f, path, 'peers_omitted', soft(nonNegativeInteger, 0, u)),
    variables_omitted: field(f, path, 'variables_omitted', soft(nonNegativeInteger, 0, u)),
    operations: field(
      f,
      path,
      'operations',
      soft(
        items((item, ip) => readOperationReport(item, ip, u), u),
        [],
        u,
      ),
    ),
    operations_from_seq: field(
      f,
      path,
      'operations_from_seq',
      soft(nullable(safeInteger), null, u),
    ),
    operations_open_omitted: field(
      f,
      path,
      'operations_open_omitted',
      soft(nonNegativeInteger, 0, u),
    ),
  };
};

const readVariables = readVariablesReport;

/** The `values` notification: the last values of the watched variables, and when they are from. */
export interface ValuesMessage {
  type: 'values';
  /** id of the watch. */
  id: number;
  watch_id: number;
  run_id: string;
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
  run_id: string;
  /** Operation number. The result arrives in `operation` notifications. */
  operation: number;
}

export const readWriteResponse = (value: unknown): WriteAnswer => {
  const f = object(value, 'response');
  return {
    run_id: field(f, 'response', 'run_id', nonEmptyString),
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
  instance?: string;
  run_id?: string;
  after_stream_seq?: number;
}

/** replay of hello: where the resend starts (null: from the beginning). */
export interface Replay {
  after_stream_seq: number | null;
}

const readReplay: Reader<Replay> = (value, path) => {
  const f = object(value, path);
  return { after_stream_seq: field(f, path, 'after_stream_seq', nullable(integer)) };
};

/** dropped: how much was not sent to this screen. */
export interface Dropped {
  type: 'dropped';
  stream_seq?: number;
  run_id: string | null;
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
  | {
      type: 'hello';
      instance: string;
      stream_seq?: number;
      session: SessionSummary | null;
      /** What the server accepts now; also there when `session` is null. null from a server that does not say. */
      allowed: ServerAllowed | null;
      replay: Replay | null;
      runtime?: RuntimeStatus | null;
    }
  | { type: 'runtime'; stream_seq?: number; runtime: RuntimeStatus }
  /**
   * What the server accepts changed with no `session` to carry it (a start begins or fails, the server begins to end).
   * run_id is the current run's id (null if none). Other changes arrive in the summary.
   */
  | { type: 'allowed'; stream_seq?: number; run_id: string | null; allowed: ServerAllowed }
  /** A launcher event (resent log lines as well as new events). */
  | { type: 'event'; stream_seq?: number; run_id: string; event: LauncherEvent }
  /** The resend has finished. */
  | { type: 'synced'; stream_seq?: number; run_id: string | null; last_seq: number | null }
  /** The session summary changed (started, stop requested, the launcher exited, the run ended). */
  | { type: 'session'; stream_seq?: number; session: SessionSummary }
  /**
   * A server notice (a gap in seq, an invalid line from the launcher, an end without a result, failed authentication
   * and so on).
   */
  | ({ type: 'error'; stream_seq?: number; run_id: string | null } & ServerProblem)
  /** How much was not sent. */
  | Dropped
  /** The screen read too slowly and messages were dropped. Everything is sent again, starting with hello. */
  | { type: 'resync'; message: string }
  /** The state or the list of the variable API changed (also once right after a run starts). */
  | { type: 'variables'; stream_seq?: number; run_id: string; variables: VariablesSummary }
  /** Values of the watched variables. */
  | ValuesMessage
  /** The result of a write changed. */
  | ({ type: 'operation'; stream_seq?: number; run_id: string } & OperationReport)
  /**
   * Whether ECUs use their variables changed. For each participant listed, the state of every variable it reports
   * (replaces what came before for that participant; a variable not listed is `active`).
   */
  | {
      type: 'availability';
      stream_seq?: number;
      run_id: string;
      peers: Record<string, Record<string, VariableAvailability>>;
    };

/**
 * Reads one WebSocket notification. Unknown notifications, and notifications wrapping an unknown event, are null
 * (skipped).
 */
export const readServerMessage = (value: unknown): ServerMessage | null => {
  const path = 'message';
  const f = object(value, path);
  const type = field(f, path, 'type', string);
  const streamSeq =
    f['stream_seq'] === undefined ? undefined : field(f, path, 'stream_seq', nonNegativeInteger);
  const seqField = streamSeq === undefined ? {} : { stream_seq: streamSeq };
  switch (type) {
    case 'hello': {
      version(f, path, 'vehicle_sil_gui', PROTOCOL_VERSION);
      const runtime = field(f, path, 'runtime', optional(nullable(readRuntimeStatus), undefined));
      return {
        type,
        ...seqField,
        instance: field(f, path, 'instance', nonEmptyString),
        session: field(f, path, 'session', nullable(readSessionSummary)),
        allowed: field(f, path, 'allowed', optional(nullable(readServerAllowed), null)),
        replay: field(f, path, 'replay', nullable(readReplay)),
        ...(runtime === undefined ? {} : { runtime }),
      };
    }
    case 'runtime':
      return { type, ...seqField, runtime: field(f, path, 'runtime', readRuntimeStatus) };
    case 'event': {
      const event = field(f, path, 'event', (v, p) => readLauncherEvent(v, p, true));
      return (
        event && { type, ...seqField, run_id: field(f, path, 'run_id', nonEmptyString), event }
      );
    }
    case 'synced':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nullable(nonEmptyString)),
        last_seq: field(f, path, 'last_seq', nullable(integer)),
      };
    case 'session':
      return { type, ...seqField, session: field(f, path, 'session', readSessionSummary) };
    case 'error':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nullable(nonEmptyString)),
        ...readServerProblem(value, path),
      };
    case 'dropped':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nullable(nonEmptyString)),
        reason: field(f, path, 'reason', string),
        log: field(f, path, 'log', record(integer)),
        at_least: field(f, path, 'at_least', array(string)),
        events: field(f, path, 'events', integer),
      };
    case 'allowed':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nullable(nonEmptyString)),
        allowed: field(f, path, 'allowed', readServerAllowed),
      };
    case 'resync':
      return { type, message: field(f, path, 'message', optional(string, '')) };
    case 'variables':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nonEmptyString),
        variables: field(f, path, 'variables', readVariables),
      };
    case 'values': {
      const watchId =
        f['watch_id'] !== undefined
          ? field(f, path, 'watch_id', integer)
          : field(f, path, 'id', integer);
      return {
        type,
        id: watchId,
        watch_id: watchId,
        run_id: field(f, path, 'run_id', nonEmptyString),
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
    }
    case 'operation':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nonEmptyString),
        ...readOperationReport(value, path),
      };
    case 'availability':
      return {
        type,
        ...seqField,
        run_id: field(f, path, 'run_id', nonEmptyString),
        peers: field(
          f,
          path,
          'peers',
          record((v, p) => record((x) => readAvailability(x))(v, p)),
        ),
      };
    default:
      return null;
  }
};
