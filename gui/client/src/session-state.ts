// Pure functions that build the state of a session (one run of the launcher) from the server's summary and the
// launcher's events. The session copy (SessionFeed in session.ts) uses them; users read the result as readable values.
import {
  CheckResult,
  CONTROL_EVENT,
  CommandReport,
  ControlLink,
  Dropped,
  Finding,
  LauncherEvent,
  PHASES,
  ParticipantState,
  Phase,
  RunResult,
  ScenarioStatus,
  ScenarioStep,
  ServerProblem,
  SessionSummary,
  SummaryScenario,
  Speed,
  StateReport,
  StopRequest,
  TimeReport,
} from './protocol';

/**
 * A reason: a code that stays the same (an entry of the owner's reason codes, or one the client names) and one English
 * sentence.
 */
export interface Reason {
  code: string;
  message: string;
}

/** What the launcher says of one operation: why it does not accept it now (null if it does), and its warning. */
export interface LauncherAnswer {
  reason: Reason | null;
  caution: Reason | null;
}

/**
 * What the launcher's last `state` says of one operation (`allowed.<name>`, such as `write` or `pause`), as it wrote
 * it. A launcher that has not said yet does not accept it (`unsupported`): no `state` yet (at the start of a run,
 * before the launcher's first one), or a `state` with no entry for the operation.
 * The one place the client reads `allowed` of the launcher: `controls` of the front door and `client.variables.bind`
 * both use it.
 */
export function launcherAnswer(state: StateReport | null, name: string): LauncherAnswer {
  if (!state) {
    return {
      reason: { code: 'unsupported', message: 'the launcher has not said yet what it accepts' },
      caution: null,
    };
  }
  const item = Object.hasOwn(state.allowed, name) ? state.allowed[name] : undefined;
  if (!item) {
    return {
      reason: {
        code: 'unsupported',
        message: `this launcher does not say whether it accepts ${name}`,
      },
      caution: null,
    };
  }
  const caution = item.caution ? { code: item.caution.code, message: item.caution.message } : null;
  if (item.ok) return { reason: null, caution };
  return {
    reason: {
      code: item.reason ?? 'refused',
      message: item.message ?? 'the launcher does not accept it now',
    },
    caution,
  };
}

/** Log lines kept per source (participant or launcher). The same number the server keeps. */
export const MAX_LOG_LINES_PER_SOURCE = 2000;
/** Log lines kept in total. Beyond this, the oldest lines of the source holding the most are dropped. */
export const MAX_LOG_LINES = 20_000;
/** Server notices kept. Beyond this, only the number is counted. */
export const MAX_PROBLEMS = 200;
/** Findings of the run kept. */
export const MAX_FINDINGS = 500;

/** One log line. */
export interface LogLine {
  /** seq of the event. A marker for dropped lines has the seq of the line before it + 0.5 (markers may share a seq). */
  seq: number;
  /** Key the screen tells lines apart by (unique within the session, markers included). */
  key: string;
  /** The participant name, or `vehicle_sil_launch`. */
  source: string;
  line: string;
  /**
   * Only on a marker for dropped lines ("N lines were dropped here"). launcher: lines the launcher could not write
   * (log_dropped).
   */
  gap?: { count: number; atLeast: boolean; launcher?: boolean };
}

/** Log counts per source. */
export interface LogSource {
  /** Lines held now (markers for dropped lines not counted). */
  lines: number;
  /** Old lines dropped (those the server no longer kept, and those the screen dropped at its limit). */
  dropped: number;
}

/** State of a participant during the run. */
export interface ParticipantView {
  name: string;
  /** The last state received. null if nothing has arrived yet. */
  state: ParticipantState | null;
  required: boolean | null;
  pid: number | null;
  exitCode: number | null;
  signal: string | null;
  /**
   * Why the launcher decided the participant failed (reason of the failed event; may arrive before the process ends).
   * Kept even if ended arrives later. null unless it was decided to have failed.
   */
  failure: string | null;
  /** Why it could not be started (not_started), or the text of the failure finding (failed). In English, unchanged. */
  message: string | null;
  /**
   * The launcher asked it to stop (stop_requested; ended in the result is stopped). How it ends after that is not
   * colored as a failure.
   */
  stopRequested: boolean;
  /** Names of fields the server left out because the summary was full (empty once a later event arrives). */
  omittedFields: readonly string[];
  /**
   * State of the connection to the ECU's variables (the control_link event, or control_link of the result). null while
   * unknown.
   */
  controlLink: ControlLink | null;
  /** A participant the user starts (manual of the check result, or the manual event). */
  manual: boolean;
  /** manual: the command line that starts it (as the launcher wrote it). null if it has not arrived. */
  commandLine: string | null;
  /** manual: the directory to run that command in. null if it has not arrived. */
  workingDirectory: string | null;
  /**
   * manual: environment variables to set when starting it (names and values, as the launcher wrote them). Empty if
   * there are none. When the registry is not on a loopback address, it includes `VEHICLE_SIL_LISTEN_ON_NETWORK` with
   * `1`.
   */
  environment: Readonly<Record<string, string>>;
}

/** State of the launcher process (as the server sees it). */
export interface LauncherView {
  /** Whether it is running. Stays true after the result arrives, until it exits. Decides whether it can be stopped. */
  running: boolean;
  exitCode: number | null;
  /** Number of the signal that ended it (POSIX only). */
  signal: number | null;
  /**
   * Seconds it has been running without writing anything (the server watches with a heartbeat). null while it writes.
   */
  silentSeconds: number | null;
}

/**
 * Virtual time and how it keeps up (the last time event, or time of the summary). Fields that are not written are
 * null. Whether the run is paused, and the speed set, are in the launcher's state (SessionView.launcherState).
 */
export interface TimeView {
  simMs: number;
  /** Actual speed over roughly the last second (null when starting to move, and in an event written while paused). */
  actualSpeed: number | null;
  /** Number of late time steps. */
  lateSteps: number | null;
  /** Max lateness (ms). */
  maxLateMs: number | null;
  /** Total slip (ms). */
  slippedMs: number | null;
  /** Lateness of a time step that is past its deadline and has not started yet (ms). */
  lateMs: number | null;
}

/** The last command event received (stop included). */
export interface CommandView {
  /** Name of the command (null for an unreadable line). */
  name: string | null;
  accepted: boolean;
  /** Why it was not accepted (in English, unchanged). */
  message: string | null;
  /** For an accepted step, the virtual time it actually advances (ms). */
  ms: number | null;
  /** For an accepted step or run_until, the virtual time at which the simulation pauses after it (ms). */
  atMs: number | null;
  /** seq of that event (-1 if unknown). Tells a new answer from the same answer arriving again in a summary. */
  seq: number;
}

export function timeView(time: TimeReport): TimeView {
  return {
    simMs: time.sim_ms,
    actualSpeed: time.actual_speed,
    lateSteps: time.late_steps,
    maxLateMs: time.max_late_ms,
    slippedMs: time.slipped_ms,
    lateMs: time.late_ms,
  };
}

function commandView(command: CommandReport, seq: number): CommandView {
  return {
    name: command.command,
    accepted: command.accepted,
    message: command.message,
    ms: command.ms,
    atMs: command.at_ms,
    seq,
  };
}

/**
 * Progress of a scenario run (from scenario of the summary, scenario_step events, and scenario of the check and the
 * result).
 */
export interface ScenarioView {
  /** The scenario file (relative to root). */
  file: string;
  name: string | null;
  /** Number of steps (from the check result; null until then). */
  stepCount: number | null;
  /** Steps received (in index order, the last state of each step). After the result, every step. */
  steps: readonly ScenarioStep[];
  /** Number of steps beyond the limit that the server did not keep. */
  stepsOmitted: number;
  /** Result of the scenario (passed, failed, incomplete, not_run). null until there is a result. */
  status: ScenarioStatus | null;
  /** Virtual time at which the scenario ended. */
  overAtMs: number | null;
  /**
   * Where the server serves the run's timeline file (`api/timeline/<session id>`), once the run has ended with one;
   * else null. The page `timeline/` of the server draws it (see "Timeline of a scenario run" in docs/gui.md).
   */
  timelineUrl: string | null;
}

function scenarioView(summary: SummaryScenario): ScenarioView {
  return {
    file: summary.file,
    name: summary.name,
    stepCount: summary.step_count,
    steps: summary.steps,
    stepsOmitted: summary.steps_omitted,
    status: summary.status,
    overAtMs: summary.over_at_ms,
    timelineUrl: summary.timeline_url,
  };
}

/** Restore a locally received complete string only when the server's value is exactly its shortened form. */
function fullStepText(incoming: string | null, current: string | null): string | null {
  if (incoming === null || current === null) return incoming;
  const cut = /^(.*)\.\.\. \((\d+) more characters\)$/s.exec(incoming);
  if (!cut) return incoming;
  const prefix = cut[1];
  return current.startsWith(prefix) &&
    Array.from(current).length === Array.from(prefix).length + Number(cut[2])
    ? current
    : incoming;
}

function mergeScenarioStep(current: ScenarioStep, incoming: ScenarioStep): ScenarioStep {
  return {
    ...incoming,
    text: fullStepText(incoming.text, current.text) ?? incoming.text,
    message: fullStepText(incoming.message, current.message),
  };
}

/** Applies one step (replacing one with the same index; kept in index order). */
function withStep(steps: readonly ScenarioStep[], step: ScenarioStep): readonly ScenarioStep[] {
  const i = steps.findIndex((s) => s.index >= step.index);
  if (i < 0) return [...steps, step];
  if (steps[i].index === step.index) return steps.map((s, j) => (j === i ? step : s));
  return [...steps.slice(0, i), step, ...steps.slice(i)];
}

/** State of one session (one run of the launcher), as the screen sees it. */
export interface SessionView {
  /** Session number the server assigned. */
  id: number;
  /** Server snapshot capture order for this run, null on older servers. */
  snapshotRev?: number | null;
  /** Highest diagnostic identifier represented in this view. */
  problemSeq?: number;
  /** Highest diagnostic delivered by the ordered WebSocket stream. */
  problemEventSeq?: number;
  /** Summary findings awaiting possible earlier buffered live events. */
  pendingFindings?: { through: number; keys: readonly string[] };
  /** Highest live or replay event observed, retained across full log replay. */
  observedSeq?: number;
  /** seq of the last event received (-1 if none). On reconnect, the resend is requested from the next one. */
  lastSeq: number;
  /** The configuration file (relative to root). */
  file: string;
  /** Participants left out. */
  disable: readonly string[];
  /**
   * Participants the user chose on the screen to start themselves (manual: true in the configuration file is in check).
   */
  manual: readonly string[];
  /** Speed requested at the start (null means no speed was requested). The current speed is `speed` of launcherState. */
  speed: Speed;
  /** Whether the run started paused. */
  startPaused: boolean;
  phase: Phase | null;
  /** Position of the last known phase received (so an unknown phase never moves the phase backwards). */
  phaseRank: number;
  check: CheckResult | null;
  participants: readonly ParticipantView[];
  /** Log lines (in seq order). */
  logs: readonly LogLine[];
  /** Per source, the number of lines held and dropped. */
  logSources: Readonly<Record<string, LogSource>>;
  /** Findings decided during the run (the check's findings are in check). */
  findings: readonly Finding[];
  /** t_ms of the last event received (real time since the launcher started). */
  elapsedMs: number;
  /** Virtual time (only once a time event has arrived). */
  simMs: number | null;
  /**
   * Names of the fields of the last time that the server left out because the summary was full (empty once a later
   * event arrives).
   */
  timeOmittedFields: readonly string[];
  /** Virtual time and how it advances (once a time event has arrived). */
  time: TimeView | null;
  /** Number of stop requests (1: "stopping"; 2: after "stop without waiting" was requested). */
  stopRequests: number;
  /** The last command event received (whether it was accepted, and why not). */
  lastCommand: CommandView | null;
  /**
   * What the launcher accepts now and where the run stands (the last `state` event, or `launcher_state` of the
   * summary; the one with the larger `rev` wins). null until one arrives (and for a launcher without `state`).
   */
  launcherState: StateReport | null;
  result: RunResult | null;
  launcher: LauncherView;
  /** What the server noticed (shown in English, unchanged). */
  problems: readonly ServerProblem[];
  /** Number of server notices beyond the limit that were not kept. */
  problemsOmitted: number;
  /**
   * Names of the parts that could not be read (`result.participants` of the summary, `finding.code` of an event and so
   * on). The screen shows "some parts cannot be displayed" for the parts listed here.
   */
  unreadable: readonly string[];
  /**
   * Events not applied because their seq was not after the previous event's (overlap with the server's resend, or seq
   * went back).
   */
  staleEvents: number;
  /** Number of markers placed for lines the server did not send (dropped); used for the markers' keys. */
  droppedMarks: number;
  /** Progress of the scenario (null for a run without a scenario). */
  scenario: ScenarioView | null;
}

/** Where the run stands now (used by the status bar and the result display). */
export type RunOutcome =
  /** Running, with no stop requested. */
  | 'running'
  /** A stop was requested (waiting for the participants to end). */
  | 'stopping'
  /** A stop without waiting was requested. */
  | 'forcing'
  /** There is a result, but the launcher is still running (waiting for it to exit). */
  | 'finishing'
  /** The launcher printed a result and exited. */
  | 'ended'
  /** The launcher exited without a result. */
  | 'crashed';

/**
 * Whether the run waits before its first step (the step at time 0) for a resume or a step: the launcher's state says
 * so (`holding`; a run started paused whose launcher has a Control Server). Meanwhile the launcher writes no time
 * events, and a variable written is applied in the step at time 0.
 */
export function isHolding(session: SessionView | null): boolean {
  return !!session && session.launcher.running && session.launcherState?.holding === true;
}

export function runOutcome(session: SessionView): RunOutcome {
  if (session.launcher.running) {
    if (session.result) return 'finishing';
    if (session.stopRequests >= 2) return 'forcing';
    return session.stopRequests >= 1 ? 'stopping' : 'running';
  }
  return session.result ? 'ended' : 'crashed';
}

/** Builds a session from the server's summary (hello, a new session). The log arrives in the resend that follows. */
export function sessionFromSummary(summary: SessionSummary): SessionView {
  const blank: SessionView = {
    id: summary.id,
    ...(summary.snapshot_rev == null ? {} : { snapshotRev: summary.snapshot_rev }),
    lastSeq: -1,
    file: summary.file,
    disable: summary.disable,
    manual: summary.manual,
    speed: summary.speed,
    startPaused: summary.start_paused,
    phase: null,
    phaseRank: -1,
    check: null,
    participants: [],
    logs: [],
    logSources: {},
    findings: [],
    elapsedMs: 0,
    simMs: null,
    timeOmittedFields: [],
    time: null,
    stopRequests: 0,
    lastCommand: null,
    launcherState: null,
    result: null,
    launcher: launcherOf(summary),
    problems: [],
    problemsOmitted: 0,
    unreadable: [],
    staleEvents: 0,
    droppedMarks: 0,
    scenario: null,
  };
  const adopted = applySummary(blank, summary, true);
  return summary.last_seq == null
    ? adopted
    : {
        ...adopted,
        pendingFindings: { through: summary.last_seq, keys: summary.findings.map(findingKey) },
      };
}

/**
 * Counts log lines that were not sent (dropped) and places an "N lines were dropped here" marker at the current
 * position of that source's log. Before the resend (not-kept), it goes before the lines being resent.
 */
export function addDropped(session: SessionView, dropped: Dropped): SessionView {
  const counts = Object.entries(dropped.log).filter(([, count]) => count > 0);
  if (counts.length === 0) return session;
  const mark = session.droppedMarks + 1;
  const gaps: LogLine[] = counts.map(([source, count]) => ({
    seq: session.lastSeq + 0.5,
    key: `dropped ${mark} ${source}`,
    source,
    line: '',
    gap: { count, atLeast: dropped.at_least.includes(source) },
  }));
  const next = { ...appendLogs(session, gaps), droppedMarks: mark };
  const sources = { ...next.logSources };
  for (const [source, count] of counts) {
    sources[source] = { ...sources[source], dropped: sources[source].dropped + count };
  }
  return { ...next, logSources: sources };
}

const STOP_REQUESTS: Record<string, number> = { none: 0, requested: 1, forced: 2 };

/** Turns the stop request into a count (an unknown value counts as requested: 1). */
function stopRequestsOf(stop: StopRequest): number {
  return STOP_REQUESTS[stop] ?? 1;
}

function launcherOf(summary: SessionSummary): LauncherView {
  return {
    running: summary.launcher.running,
    exitCode: summary.launcher.exit_code,
    signal: summary.launcher.signal,
    silentSeconds: summary.launcher.silent_s,
  };
}

/**
 * Applies a summary. The local log is kept.
 *
 * - force (WebSocket hello and session): arrives in the same order as events, so everything is taken as correct at
 *   the time it arrives.
 * - Without force (an HTTP answer): its order relative to notifications is unknown. Whether the launcher runs and the
 *   stop requests are taken, but a launcher that has ended in the same run is never set back to running. Server
 *   notices are taken only when there are no fewer than held locally (the server only adds, so a summary with fewer is
 *   old). Phase, check, participants, findings, result and virtual time are taken only when the summary is newer than
 *   the local events, and not at all with controlOnly (the WebSocket is connected and the same things arrive in
 *   notifications), so findings do not appear twice, once from the summary and once from a notification.
 */
export function applySummary(
  session: SessionView,
  summary: SessionSummary,
  force = false,
  controlOnly = false,
): SessionView {
  const incomingRev = summary.snapshot_rev ?? null;
  const currentRev = session.snapshotRev ?? null;
  if (currentRev !== null && (incomingRev === null || incomingRev < currentRev)) return session;
  const newer = force || (summary.last_seq ?? -1) > session.lastSeq;
  const launcher =
    !force && !session.launcher.running && summary.launcher.running
      ? session.launcher
      : launcherOf(summary);
  const notLess =
    summary.problems.length + summary.problems_omitted >=
    session.problems.length + session.problemsOmitted;
  const mergedProblems = [...summary.problems];
  let omitted = summary.problems_omitted;
  if (summary.problem_seq != null) {
    const included = new Set(mergedProblems.map((problem) => problem.problem_seq));
    for (const problem of session.problems) {
      if (problem.problem_seq === undefined || included.has(problem.problem_seq)) continue;
      if (mergedProblems.length >= MAX_PROBLEMS) break;
      mergedProblems.push(problem);
      if (problem.problem_seq <= summary.problem_seq && omitted > 0) omitted--;
    }
  }
  if (mergedProblems.every((problem) => problem.problem_seq !== undefined))
    mergedProblems.sort((a, b) => a.problem_seq! - b.problem_seq!);
  const next: SessionView = {
    ...session,
    ...(incomingRev === null ? {} : { snapshotRev: incomingRev }),
    ...(summary.problem_seq == null
      ? {}
      : { problemSeq: Math.max(session.problemSeq ?? 0, summary.problem_seq) }),
    launcher,
    stopRequests: Math.max(session.stopRequests, stopRequestsOf(summary.stop)),
    logSources: withSources(session.logSources, Object.keys(summary.log)),
    ...(force || notLess ? { problems: mergedProblems, problemsOmitted: omitted } : {}),
  };
  if (!newer || (!force && controlOnly)) {
    return next;
  }
  const phase = summary.phase ?? next.phase;
  const rank = phaseIndex(phase);
  const preserveEvents = force && controlOnly;
  const check =
    (preserveEvents ? next.check : null) ??
    summary.check ??
    next.check ??
    summary.result?.check ??
    null;
  const result = (preserveEvents ? next.result : null) ?? summary.result ?? next.result;
  const known = new Map(session.participants.map((p) => [p.name, p]));
  const participants: readonly ParticipantView[] = summary.participants.map((p) => ({
    name: p.name,
    state: p.state,
    required: null,
    pid: p.pid,
    exitCode: p.exit_code,
    signal: p.signal,
    failure: p.reason,
    message: p.message ?? (preserveEvents ? (known.get(p.name)?.message ?? null) : null),
    // A stop the launcher asked for is kept even in summaries of later states (ended and so on)
    stopRequested:
      p.stop_requested || p.state === 'stop_requested' || !!known.get(p.name)?.stopRequested,
    omittedFields: p.omitted_fields,
    controlLink: p.control_link ?? known.get(p.name)?.controlLink ?? null,
    manual: p.state === 'manual' || p.command_line !== null || !!known.get(p.name)?.manual,
    commandLine:
      p.command_line ?? (preserveEvents ? (known.get(p.name)?.commandLine ?? null) : null),
    workingDirectory:
      p.working_directory ??
      (preserveEvents ? (known.get(p.name)?.workingDirectory ?? null) : null),
    environment: p.environment ?? (preserveEvents ? (known.get(p.name)?.environment ?? {}) : {}),
  }));
  const scenario = summary.scenario ? scenarioView(summary.scenario) : next.scenario;
  const mergedScenario =
    preserveEvents && scenario && next.scenario
      ? {
          ...scenario,
          steps: scenario.steps.reduce((steps, step) => {
            const old = next.scenario?.steps.find((item) => item.index === step.index);
            return withStep(steps, old ? mergeScenarioStep(old, step) : step);
          }, next.scenario.steps),
        }
      : scenario;
  if (mergedScenario && scenario && mergedScenario !== scenario) {
    mergedScenario.stepsOmitted = Math.max(
      0,
      scenario.stepsOmitted - (mergedScenario.steps.length - scenario.steps.length),
    );
  }
  const mergedParticipants = preserveEvents
    ? [
        ...participants,
        ...next.participants.filter(
          (p) => !participants.some((current) => current.name === p.name),
        ),
      ]
    : participants;
  return {
    ...next,
    unreadable: force ? summary.unreadable : union(next.unreadable, summary.unreadable),
    phase,
    phaseRank: rank >= 0 ? rank : next.phaseRank,
    elapsedMs: Math.max(next.elapsedMs, summary.last_t_ms ?? 0),
    simMs: summary.sim_ms ?? next.simMs,
    timeOmittedFields:
      summary.sim_ms === null ? next.timeOmittedFields : summary.time_omitted_fields,
    time: summary.time ? timeView(summary.time) : next.time,
    lastCommand: summary.last_command
      ? commandView(summary.last_command, summary.last_command.seq ?? -1)
      : next.lastCommand,
    launcherState: newerState(next.launcherState, summary.launcher_state ?? null),
    findings: preserveEvents ? mergeFindings(next.findings, summary.findings) : summary.findings,
    result,
    check,
    scenario: mergedScenario,
    // Whether a participant is required or left out is in the check result (as is the order, that of the configuration
    // file). If there is a result, how it ended (stopped or not) comes from the result too
    participants: withResult(
      check ? mergeCheck(mergedParticipants, check) : mergedParticipants,
      result,
    ),
  };
}

/** The state with the larger rev (a state sent again, or an old one in a resend, never replaces a newer one). */
function newerState(current: StateReport | null, incoming: StateReport | null): StateReport | null {
  if (!incoming) return current;
  return !current || incoming.rev >= current.rev ? incoming : current;
}

function union(a: readonly string[], b: readonly string[]): readonly string[] {
  return b.every((name) => a.includes(name)) ? a : [...new Set([...a, ...b])];
}

/** Adds sources that have no lines yet to the list of sources (so they can be selected). */
function withSources(
  sources: Readonly<Record<string, LogSource>>,
  names: readonly string[],
): Readonly<Record<string, LogSource>> {
  const missing = names.filter((name) => !(name in sources));
  if (missing.length === 0) return sources;
  const next = { ...sources };
  for (const name of missing) next[name] = { lines: 0, dropped: 0 };
  return next;
}

/**
 * Adds a server notice (later summaries carry the same one and replace it). Beyond the limit, only the number is
 * counted.
 */
export function addProblem(session: SessionView, problem: ServerProblem): SessionView {
  const seq = problem.problem_seq;
  if (seq !== undefined && seq <= (session.problemEventSeq ?? 0)) return session;
  const updated = seq === undefined ? session : { ...session, problemEventSeq: seq };
  if (seq !== undefined && session.problems.some((item) => item.problem_seq === seq))
    return updated;
  if (updated.problems.length >= MAX_PROBLEMS) {
    const alreadyCounted = seq !== undefined && seq <= (session.problemSeq ?? 0);
    return alreadyCounted ? updated : { ...updated, problemsOmitted: updated.problemsOmitted + 1 };
  }
  const alreadyCounted = seq !== undefined && seq <= (session.problemSeq ?? 0);
  const problems = [...updated.problems, problem];
  if (problems.every((item) => item.problem_seq !== undefined))
    problems.sort((a, b) => a.problem_seq! - b.problem_seq!);
  return {
    ...updated,
    problems,
    problemsOmitted: alreadyCounted
      ? Math.max(0, updated.problemsOmitted - 1)
      : updated.problemsOmitted,
  };
}

/** Position of the phase (-1 for an unknown phase). */
export function phaseIndex(phase: Phase | null): number {
  return phase === null ? -1 : (PHASES as readonly string[]).indexOf(phase);
}

/** The fields common to a finding in a summary and in a live event. */
function findingKey(finding: Finding): string {
  return JSON.stringify([
    finding.severity,
    finding.code,
    finding.file,
    finding.line,
    finding.participant,
    finding.network,
    finding.message,
    finding.detail,
  ]);
}

/** Merge two ordered, potentially incomplete lists without coalescing repeated findings. */
function mergeFindings(
  current: readonly Finding[],
  incoming: readonly Finding[],
): readonly Finding[] {
  const a = current.slice(0, MAX_FINDINGS);
  const b = incoming.slice(0, MAX_FINDINGS);
  const ak = a.map(findingKey);
  const bk = b.map(findingKey);
  const sizes = Array.from({ length: a.length + 1 }, () => new Uint16Array(b.length + 1));
  for (let i = a.length - 1; i >= 0; --i)
    for (let j = b.length - 1; j >= 0; --j)
      sizes[i][j] =
        ak[i] === bk[j] ? 1 + sizes[i + 1][j + 1] : Math.max(sizes[i + 1][j], sizes[i][j + 1]);
  const merged: Finding[] = [];
  let i = 0,
    j = 0;
  while ((i < a.length || j < b.length) && merged.length < MAX_FINDINGS) {
    if (i < a.length && j < b.length && ak[i] === bk[j]) {
      merged.push(a[i++]);
      j++;
    } else if (j === b.length || (i < a.length && sizes[i + 1][j] >= sizes[i][j + 1]))
      merged.push(a[i++]);
    else merged.push(b[j++]);
  }
  return merged;
}

/**
 * Applies a batch of events. Events with the same or an earlier seq are not applied (overlap with the resend: what
 * arrived before the reconnect). With countStale (events that are not a resend), those not applied are counted in
 * staleEvents (seq went back). The log array is rebuilt once per batch (not copied for every event).
 */
export function applyEvents(
  session: SessionView,
  events: readonly LauncherEvent[],
  countStale = false,
): SessionView {
  let next = session;
  const logs: LogLine[] = [];
  let stale = 0;
  for (const event of events) {
    if (event.seq > next.lastSeq) {
      next = { ...next, observedSeq: Math.max(next.observedSeq ?? -1, event.seq) };
      const pending = next.pendingFindings;
      if (event.event === 'finding' && pending && event.seq <= pending.through) {
        const index = pending.keys.indexOf(findingKey(event.finding));
        if (index !== -1) {
          const keys = [...pending.keys];
          keys.splice(index, 1);
          next = { ...next, lastSeq: event.seq, pendingFindings: { ...pending, keys } };
          continue;
        }
      }
      next = applyOne(next, event, logs);
      if (event.unreadable)
        next = { ...next, unreadable: union(next.unreadable, event.unreadable) };
    } else {
      stale++;
    }
  }
  if (countStale && stale > 0) next = { ...next, staleEvents: next.staleEvents + stale };
  if (logs.length === 0) return next;
  next = appendLogs(next, logs);
  // Lines the launcher could not write (log_dropped) count as dropped lines of that source
  const gaps = logs.filter((line) => line.gap?.launcher);
  if (gaps.length === 0) return next;
  const sources = { ...next.logSources };
  for (const gap of gaps) {
    const counts = sources[gap.source] ?? { lines: 0, dropped: 0 };
    sources[gap.source] = { ...counts, dropped: counts.dropped + (gap.gap?.count ?? 0) };
  }
  return { ...next, logSources: sources };
}

/**
 * Adds log lines and drops the oldest lines beyond the per-source limit and the total limit. Markers for dropped lines
 * do not count as lines, and markers before the lines being dropped go with them (their position is no longer
 * visible).
 */
function appendLogs(session: SessionView, added: readonly LogLine[]): SessionView {
  const sources: Record<string, LogSource> = {};
  for (const [source, counts] of Object.entries(session.logSources))
    sources[source] = { ...counts };
  for (const line of added) {
    const counts = (sources[line.source] ??= { lines: 0, dropped: 0 });
    if (!line.gap) counts.lines++;
  }
  // Decide how many lines to drop from each source
  const drop = new Map<string, number>();
  const take = (source: string, count: number) => {
    sources[source].lines -= count;
    sources[source].dropped += count;
    drop.set(source, (drop.get(source) ?? 0) + count);
  };
  let total = 0;
  for (const [source, counts] of Object.entries(sources)) {
    if (counts.lines > MAX_LOG_LINES_PER_SOURCE)
      take(source, counts.lines - MAX_LOG_LINES_PER_SOURCE);
    total += counts.lines;
  }
  while (total > MAX_LOG_LINES) {
    const [largest, second] = Object.entries(sources).sort(([, a], [, b]) => b.lines - a.lines);
    const count = Math.max(
      1,
      Math.min(total - MAX_LOG_LINES, largest[1].lines - (second?.[1].lines ?? 0)),
    );
    take(largest[0], count);
    total -= count;
  }
  const all = [...session.logs, ...added];
  if (drop.size === 0) return { ...session, logs: all, logSources: sources };
  const kept = all.filter((line) => {
    const left = drop.get(line.source);
    if (left === undefined || left === 0) return true;
    if (!line.gap) drop.set(line.source, left - 1);
    return false;
  });
  return { ...session, logs: kept, logSources: sources };
}

function applyOne(session: SessionView, event: LauncherEvent, logs: LogLine[]): SessionView {
  const next: SessionView = {
    ...session,
    lastSeq: event.seq,
    elapsedMs: Math.max(session.elapsedMs, event.t_ms ?? 0),
  };
  switch (event.event) {
    case 'phase': {
      // The phase never goes back (when a summary already showed a later phase, an old resent event does not move it
      // back). An unknown phase is shown as is, but a known phase that comes later is compared with the last known
      // phase
      const index = phaseIndex(event.phase);
      if (index < 0) return { ...next, phase: event.phase };
      return index >= next.phaseRank ? { ...next, phase: event.phase, phaseRank: index } : next;
    }
    case 'check': {
      // A check that could not be read (null) is not applied (it is reported as an unreadable part)
      if (!event.check) return next;
      const found = event.check.scenario;
      return {
        ...next,
        check: event.check,
        participants: mergeCheck(next.participants, event.check),
        scenario:
          next.scenario && found
            ? { ...next.scenario, name: found.name, stepCount: found.steps }
            : next.scenario,
      };
    }
    case 'scenario_step':
      // The scenario run's summary (scenario) has arrived first. Without it, nothing is applied
      return next.scenario
        ? {
            ...next,
            scenario: { ...next.scenario, steps: withStep(next.scenario.steps, event.step) },
          }
        : next;
    case 'participant':
      return { ...next, participants: updateParticipant(next.participants, event) };
    case CONTROL_EVENT: {
      const known = next.participants.find((p) => p.name === event.name);
      const updated = { ...(known ?? blankParticipant(event.name)), controlLink: event.link };
      return {
        ...next,
        participants: known
          ? next.participants.map((p) => (p === known ? updated : p))
          : [...next.participants, updated],
      };
    }
    case 'log':
      logs.push({ seq: event.seq, key: String(event.seq), source: event.source, line: event.line });
      return next;
    case 'log_dropped':
      // Place a marker where the lines were dropped ("N lines were dropped here"). The marker's seq is this event's seq
      for (const [source, count] of Object.entries(event.sources)) {
        if (count > 0) {
          logs.push({
            seq: event.seq,
            key: `${event.seq} ${source}`,
            source,
            line: '',
            gap: { count, atLeast: false, launcher: true },
          });
        }
      }
      return next;
    case 'finding':
      // The resend does not resend findings (the summary holds them), so a finding received is added as is (the same
      // text may appear twice)
      return next.findings.length >= MAX_FINDINGS
        ? next
        : { ...next, findings: [...next.findings, event.finding] };
    case 'time':
      return { ...next, simMs: event.sim_ms, timeOmittedFields: [], time: timeView(event) };
    case 'command':
      return { ...next, lastCommand: commandView(event, event.seq) };
    case 'state':
      return { ...next, launcherState: newerState(next.launcherState, event) };
    case 'result':
      return {
        ...next,
        result: event.result,
        phase: 'ended',
        phaseRank: phaseIndex('ended'),
        check: next.check ?? event.result.check,
        participants: mergeResult(next.participants, event.result),
        scenario:
          next.scenario && event.result.scenario
            ? {
                ...next.scenario,
                name: event.result.scenario.name,
                stepCount: next.scenario.stepCount ?? event.result.scenario.steps.length,
                steps: event.result.scenario.steps,
                status: event.result.scenario.status,
                overAtMs: event.result.scenario.over_at_ms,
              }
            : next.scenario,
      };
    default:
      return next;
  }
}

/**
 * Decides the order of participants (that of the configuration file) and whether each is required from the check
 * result. Keeps the states received.
 */
function mergeCheck(
  current: readonly ParticipantView[],
  check: CheckResult,
): readonly ParticipantView[] {
  const byName = new Map(current.map((p) => [p.name, p]));
  const ordered: ParticipantView[] = [];
  for (const p of check.participants) {
    if (p.name === null) continue;
    const known = byName.get(p.name);
    byName.delete(p.name);
    ordered.push({
      ...(known ?? blankParticipant(p.name)),
      required: p.required,
      state: known?.state ?? (p.enabled === false ? 'disabled' : null),
      manual: p.manual || !!known?.manual,
    });
  }
  return [...ordered, ...byName.values()];
}

function updateParticipant(
  current: readonly ParticipantView[],
  event: Extract<LauncherEvent, { event: 'participant' }>,
): readonly ParticipantView[] {
  const known = current.find((p) => p.name === event.name);
  const updated: ParticipantView = {
    ...(known ?? blankParticipant(event.name)),
    state: event.state,
    // Events arrive whole (only the summary leaves fields out)
    omittedFields: [],
  };
  if (event.pid !== null) updated.pid = event.pid;
  if (event.state === 'failed') updated.failure = event.reason ?? '';
  if (event.state === 'stop_requested') updated.stopRequested = true;
  if (event.message) updated.message = event.message;
  if (event.state === 'manual') updated.manual = true;
  // Only the manual event carries these (later events such as joined lack the fields, so keep the earlier values)
  if (typeof event.command_line === 'string') updated.commandLine = event.command_line;
  if (typeof event.working_directory === 'string')
    updated.workingDirectory = event.working_directory;
  if (event.state === 'manual') updated.environment = event.environment ?? {};
  if (event.state === 'ended' || event.state === 'killed') {
    updated.exitCode = event.exit_code;
    updated.signal = event.signal;
  }
  return known ? current.map((p) => (p === known ? updated : p)) : [...current, updated];
}

function withResult(
  participants: readonly ParticipantView[],
  result: RunResult | null,
): readonly ParticipantView[] {
  return result ? mergeResult(participants, result) : participants;
}

/**
 * Reflects how each participant ended from the final result (complete from the result alone, even if events were
 * missed).
 */
function mergeResult(
  current: readonly ParticipantView[],
  result: RunResult,
): readonly ParticipantView[] {
  const merged = [...current];
  for (const p of result.participants) {
    const i = merged.findIndex((known) => known.name === p.name);
    const base = i >= 0 ? merged[i] : blankParticipant(p.name);
    const state: ParticipantState = !p.enabled
      ? 'disabled'
      : p.ended === 'killed'
        ? 'killed'
        : p.ended === 'not_started'
          ? (base.state ?? 'not_started')
          : 'ended';
    const view: ParticipantView = {
      ...base,
      required: p.required,
      state,
      exitCode: p.exit_code,
      signal: p.signal,
      stopRequested: base.stopRequested || p.ended === 'stopped',
      // Keep what the events told about the connection (number of variables and so on); without it, only the result's
      // state. The result is newer (it has the count of messages that arrived late). What the result lacks comes from
      // the events
      controlLink: p.control_link ?? base.controlLink,
      manual: base.manual || p.manual,
    };
    if (i >= 0) merged[i] = view;
    else merged.push(view);
  }
  return merged;
}

function blankParticipant(name: string): ParticipantView {
  return {
    name,
    state: null,
    required: null,
    pid: null,
    exitCode: null,
    signal: null,
    failure: null,
    message: null,
    stopRequested: false,
    omittedFields: [],
    controlLink: null,
    manual: false,
    commandLine: null,
    workingDirectory: null,
    environment: {},
  };
}
