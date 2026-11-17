/**
 * The shapes the GUI server decides: the HTTP API (`api/*`), the WebSocket messages and the dashboard files. Parts that
 * come from the launcher (check, result, events) have the shapes of launcher.ts. Rules of the parsers: read.ts.
 */

import {
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
export const PROTOCOL_VERSION = 1;

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
  /** The variant of the configuration file (`--variant`); omitted or null means none. */
  variant?: string | null;
  /** Values of the scenario's parameters (name to value; `--param`); only with a `scenario`. */
  params?: Record<string, string | number>;
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

/**
 * What this server accepts now (`allowed` of the summary, of `hello` and of `api/session`). start: reasons
 * shutting-down, starting, already-running, launcher-unusable. stop and force: shutting-down, not-running,
 * already-stopping (stop), already-forced (force). What the launcher accepts is `launcher_state.allowed`.
 */
export interface ServerAllowed {
  start: ServerAllowedItem;
  stop: ServerAllowedItem;
  force: ServerAllowedItem;
}

const readServerAllowedItem: Reader<ServerAllowedItem> = (value, path) => {
  const f = object(value, path);
  return {
    ok: field(f, path, 'ok', boolean),
    reason: field(f, path, 'reason', optional(nullable(string), null)),
    message: field(f, path, 'message', optional(nullable(string), null)),
  };
};

export const readServerAllowed: Reader<ServerAllowed> = (value, path) => {
  const f = object(value, path);
  return {
    start: field(f, path, 'start', readServerAllowedItem),
    stop: field(f, path, 'stop', readServerAllowedItem),
    force: field(f, path, 'force', readServerAllowedItem),
  };
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

/** What the server accepts, from `allowed` of an `api/session` response: also there when there is no run. */
export const readSessionAllowed = (value: unknown): ServerAllowed | null =>
  field(
    object(value, 'response'),
    'response',
    'allowed',
    optional(nullable(readServerAllowed), null),
  );

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
export type TimeCommand = (
  | { command: 'pause' }
  | { command: 'resume' }
  /** Only while paused. Advances by ms and pauses again (the launcher rounds up to a multiple of the time step). */
  | { command: 'step'; ms: number }
  | { command: 'speed'; value: number | 'max' }
) & {
  /**
   * A whole number from 0 to 2^53 of the sender's choice. The launcher returns it as `id` of the command event that
   * answers this command, so the sender tells that answer from the answers to other commands. Needs a launcher with
   * the feature `state`.
   */
  id?: number;
};

/** Answer of POST api/time: the number of the run it was sent to, and the command passed to the launcher. */
export interface TimeAnswer {
  session_id: number;
  command: TimeCommandName;
  /** The `id` the request had (null if none). */
  id: number | null;
}

export const readTimeResponse = (value: unknown): TimeAnswer => {
  const path = 'response';
  const f = object(value, path);
  const command = field(f, path, 'command', object);
  return {
    session_id: field(f, path, 'session_id', integer),
    command: field(command, `${path}.command`, 'command', string) as TimeCommandName,
    id: field(command, `${path}.command`, 'id', optional(nullable(integer), null)),
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
          applied_writes: field(t, p, 'applied_writes', optional(nullable(integer), null)),
          failed_write: field(t, p, 'failed_write', optional(nullable(integer), null)),
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
  | {
      type: 'hello';
      instance: string;
      session: SessionSummary | null;
      /** What the server accepts now; also there when `session` is null. null from a server that does not say. */
      allowed: ServerAllowed | null;
      replay: Replay | null;
    }
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
        allowed: field(f, path, 'allowed', optional(nullable(readServerAllowed), null)),
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
