// Pure functions that build the front door's values (front.ts) from a snapshot of the client's state. The owners of
// the facts are the launcher (`state`: what the run accepts, whether it is paused), the GUI server (`allowed`: start,
// stop, force) and the connection. Nothing here is a new rule: a reason the launcher or the server wrote is passed on
// as it is, and the client writes a reason only for what it owns (the connection, a request of its own that has not
// been answered, an operation the run cannot take because there is no run).
import type { ConnectionState } from './connection';
import type {
  AppInfo,
  LauncherAllowedItem,
  RunResult,
  ServerAllowed,
  ServerAllowedItem,
} from './protocol';
import { SessionView, runOutcome } from './session-state';
import type {
  Control,
  EndKind,
  FrontClock,
  FrontControls,
  FrontResult,
  FrontStatus,
  Link,
  OperationName,
  Pacing,
  Reason,
  RunState,
} from './front-types';

/** The feature of the launcher that the GUI server lists once it passes `run_until` on. */
export const FEATURE_RUN_UNTIL = 'stdin-run-until';

/** Everything the composition reads. */
export interface Snapshot {
  connection: ConnectionState;
  /** How many times the socket closed before it opened. */
  failures: number;
  /** The socket was closed at least once since it last received a notification. */
  closedBefore: boolean;
  rejected: boolean;
  /** Why the versions differ (null if they match). */
  incompatible: string | null;
  /** The session copy matches the server. */
  ready: boolean;
  session: SessionView | null;
  /** What the GUI server accepts (null until it says). */
  allowed: ServerAllowed | null;
  limits: AppInfo['limits'];
  features: readonly string[];
  /** Operations of this page whose request has not been answered yet. */
  busy: ReadonlySet<OperationName>;
}

// --- link, run, result ----------------------------------------------------------------------------

export function linkOf(s: Snapshot): Link {
  if (s.rejected) return 'rejected';
  if (s.incompatible !== null) return 'incompatible';
  if (s.connection === 'open') return s.ready ? 'online' : 'connecting';
  return s.connection === 'closed' || s.failures > 0 || s.closedBefore ? 'offline' : 'connecting';
}

const NOT_PACED: Pacing = 'not-paced';

/** Whether a start is on its way: this page's request, or another page's (the server says `starting`). */
function startOnItsWay(s: Snapshot): boolean {
  return s.busy.has('start') || s.allowed?.start.reason === 'starting';
}

export function runOf(s: Snapshot): RunState {
  const session = s.session;
  const starting = startOnItsWay(s);
  if (!session) return starting ? 'starting' : 'none';
  const outcome = runOutcome(session);
  if (outcome === 'crashed') return starting ? 'starting' : 'crashed';
  if (outcome === 'ended' || outcome === 'finishing') return starting ? 'starting' : 'ended';
  if (outcome === 'stopping' || outcome === 'forcing') return 'stopping';
  const state = session.launcherState;
  const phase = state?.phase || session.phase;
  const paused = state ? state.paused : (session.time?.paused ?? null);
  switch (phase) {
    case null:
    case 'checking':
    case 'starting':
      return 'starting';
    case 'waiting':
      return 'waiting';
    case 'stopping':
      return 'stopping';
    case 'ended':
      return 'ended';
    default:
      return paused === true ? 'paused' : 'running';
  }
}

/** Whether the operator's stop made the run end (a stop is not a failure). */
function stoppedByOperator(result: RunResult): boolean {
  const by = (result.interrupted_by ?? '').toLowerCase();
  return result.result === 'interrupted' && by.includes('stop');
}

/**
 * The names of the results, kept here until the launcher writes `ok` and `message` into the result (then this table
 * goes: the result is passed on as written).
 */
export function resultOf(result: RunResult): FrontResult {
  const scenario = result.scenario?.status ?? null;
  const firstError = result.findings.find((f) => f.severity === 'error')?.message;
  let end: EndKind;
  let message: string;
  switch (result.result) {
    case 'passed':
      end = 'completed';
      message = 'the run reached its end';
      break;
    case 'interrupted':
      if (stoppedByOperator(result)) {
        end = 'stopped';
        message = 'the run was stopped';
      } else {
        end = 'interrupted';
        message = `the run was interrupted (${result.interrupted_by ?? 'by the system'}): start it again`;
      }
      break;
    case 'setup_failed':
      end = 'setup_failed';
      message = 'the launcher could not set the run up: read the log of vehicle_sil_launch';
      break;
    case 'check_failed':
      end = 'failed';
      message =
        'the check found errors, so no run was made: fix the findings of the check, then start again';
      break;
    default:
      end = 'failed';
      message = firstError
        ? `the run failed: ${firstError}`
        : 'the run failed: see the findings and the log of the run';
  }
  let ok = end === 'completed' || end === 'stopped';
  if (scenario === 'failed') {
    ok = false;
    message = 'the scenario failed: see its steps';
  } else if (scenario === 'incomplete' && ok) {
    message = 'the run ended before the scenario was through';
  }
  return { end, scenario, ok, message };
}

export function resultFor(s: Snapshot, run: RunState): FrontResult | null {
  const session = s.session;
  if (!session) return null;
  if (run === 'crashed') {
    const how = session.launcher.signal
      ? `signal ${session.launcher.signal}`
      : session.launcher.exitCode !== null
        ? `exit code ${session.launcher.exitCode}`
        : 'no exit code';
    return {
      end: 'crashed',
      scenario: null,
      ok: false,
      message: `the launcher ended without a result (${how}): read the log of vehicle_sil_launch, then start again`,
    };
  }
  return run === 'ended' && session.result ? resultOf(session.result) : null;
}

export function statusOf(s: Snapshot, initialFile: string | null): Omit<FrontStatus, 'problem'> {
  const run = runOf(s);
  return {
    link: linkOf(s),
    run,
    file: s.session?.file ?? initialFile,
    result: resultFor(s, run),
  };
}

// --- clock ----------------------------------------------------------------------------------------

export function clockOf(s: Snapshot): FrontClock {
  const session = s.session;
  const state = session?.launcherState ?? null;
  const time = session?.time ?? null;
  const live = !!session && session.launcher.running;
  const pacing = state?.pacing;
  return {
    sim_ms: time?.simMs ?? session?.simMs ?? null,
    speed: state?.speed ?? time?.speed ?? null,
    actual_speed: time?.actualSpeed ?? null,
    pacing: !live
      ? NOT_PACED
      : pacing === 'late'
        ? 'late'
        : pacing === 'on-time'
          ? 'on-time'
          : NOT_PACED,
  };
}

// --- controls -------------------------------------------------------------------------------------

const reason = (code: string, message: string): Reason => ({ code, message });

function control(ok: boolean, why: Reason | null = null, caution: Reason | null = null): Control {
  return { ok, reason: ok ? null : why, caution };
}

/** Why the connection stops an operation (null if it does not). Owned by the client. */
function linkReason(link: Link, s: Snapshot): Reason | null {
  switch (link) {
    case 'online':
      return null;
    case 'rejected':
      return reason(
        'rejected',
        'the server refused the token: open the URL that the server printed again (it ends with #token=...)',
      );
    case 'incompatible':
      return reason(
        'incompatible',
        s.incompatible ?? 'the server speaks another version of the GUI protocol',
      );
    case 'offline':
      return reason(
        'offline',
        'the connection to the GUI server is down: the client connects again by itself, then try again',
      );
    default:
      return reason(
        'connecting',
        'the client is still connecting to the GUI server: try again in a moment',
      );
  }
}

const busyReason = (name: string) =>
  reason('busy', `the previous ${name} has not been answered yet: wait for it`);

/** An entry of the GUI server's `allowed`, passed on. */
function fromServer(item: ServerAllowedItem, fallback: string): Control {
  return item.ok
    ? control(true)
    : control(false, reason(item.reason ?? 'refused', item.message ?? fallback));
}

/** An entry of the launcher's `allowed`, passed on (with its caution). */
function fromLauncher(item: LauncherAllowedItem): Control {
  const caution = item.caution ? reason(item.caution.code, item.caution.message) : null;
  return item.ok
    ? control(true, null, caution)
    : control(
        false,
        reason(item.reason ?? 'refused', item.message ?? 'the launcher does not accept it now'),
        caution,
      );
}

function numberOf(item: LauncherAllowedItem | undefined, key: string): number | null {
  const value = item?.raw?.[key];
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

const STATE_ITEM_MISSING = (name: string) =>
  reason(
    'unsupported',
    `this launcher does not say whether it accepts ${name} (its state event has no such entry): build a newer vehicle_sil_launch`,
  );

export function controlsOf(s: Snapshot, run: RunState): FrontControls {
  const link = linkOf(s);
  const down = linkReason(link, s);
  const session = s.session;
  const live = !!session && session.launcher.running && run !== 'ended' && run !== 'crashed';
  const state = session?.launcherState ?? null;
  const stopsAsked = session?.stopRequests ?? 0;

  // start: the GUI server owns it. The client adds the connection and its own request on its way.
  let start: Control;
  if (link === 'rejected' || link === 'incompatible') start = control(false, down);
  else if (s.busy.has('start')) start = control(false, busyReason('start'));
  else if (down) start = control(false, down);
  else if (s.allowed) start = fromServer(s.allowed.start, 'the server does not start a run now');
  else
    start = live
      ? control(
          false,
          reason('already-running', 'a run is going: stop it before you start another'),
        )
      : control(true);

  // stop: whatever the connection says, as long as the last known state is a run (or a start is on its way)
  const stoppable = s.busy.has('start') || live || run === 'starting';
  const serverStop = s.allowed?.stop;
  let stop: Control;
  if (s.busy.has('stop')) stop = control(false, busyReason('stop'));
  else if (s.busy.has('start')) stop = control(true);
  else if (serverStop && !serverStop.ok && serverStop.reason !== 'not-running')
    stop = fromServer(serverStop, 'the server does not stop the run now');
  else if (!stoppable)
    stop = control(false, reason('not-running', 'no run is going: there is nothing to stop'));
  else if (stopsAsked > 0)
    stop = control(
      false,
      reason('already-stopping', 'a stop has been asked for: wait, or force the stop'),
    );
  else stop = control(true);

  // force_stop: also the server's, and only after a first stop (it is the second step)
  const serverForce = s.allowed?.force;
  let force: Control;
  if (s.busy.has('force_stop')) force = control(false, busyReason('force stop'));
  else if (serverForce && !serverForce.ok && serverForce.reason !== 'not-running')
    force = fromServer(serverForce, 'the server does not force a stop now');
  else if (!stoppable)
    force = control(false, reason('not-running', 'no run is going: there is nothing to stop'));
  else if (stopsAsked === 0 && !s.busy.has('stop'))
    force = control(
      false,
      reason('not-stopping', 'ask for a stop first: force the stop only if it does not end'),
    );
  else force = control(true);

  // Time operations: the launcher owns them (`state.allowed`); the client adds the connection, its own request on its
  // way, and (for pause and resume) whether it would change anything
  const timeControl = (
    name: 'pause' | 'resume' | 'step' | 'speed' | 'run_until' | 'write',
    label: string,
  ): Control => {
    if (link === 'rejected' || link === 'incompatible') return control(false, down);
    if (name !== 'write' && s.busy.has(name)) return control(false, busyReason(label));
    if (down) return control(false, down);
    if (!live) return control(false, reason('not-running', 'no run is going: start one first'));
    if (!state) {
      return control(
        false,
        reason(
          'unsupported',
          'this launcher does not say what it accepts (no state event): build a newer vehicle_sil_launch',
        ),
      );
    }
    const item = state.allowed[name];
    if (!item) return control(false, STATE_ITEM_MISSING(name));
    return fromLauncher(item);
  };

  const withReason = (base: Control, code: string, message: string): Control =>
    base.ok ? control(false, reason(code, message), base.caution) : base;
  const paused = state ? state.paused : (session?.time?.paused ?? null);

  const pauseBase = timeControl('pause', 'pause');
  const pause =
    paused === true
      ? withReason(pauseBase, 'paused', 'the simulation is already paused')
      : pauseBase;
  const resumeBase = timeControl('resume', 'resume');
  const resume =
    paused === false
      ? withReason(resumeBase, 'not-paused', 'the simulation is not paused')
      : resumeBase;

  const stepItem = state?.allowed['step'];
  const speedItem = state?.allowed['speed'];
  const runUntilBase = timeControl('run_until', 'run until');
  const runUntil = s.features.includes(FEATURE_RUN_UNTIL)
    ? runUntilBase
    : control(
        false,
        reason(
          'unsupported',
          'this GUI server does not send run_until yet: use step to reach a time',
        ),
        runUntilBase.caution,
      );

  return {
    start,
    stop,
    force_stop: force,
    pause,
    resume,
    step: {
      ...timeControl('step', 'step'),
      min_ms: numberOf(stepItem, 'min_ms') ?? 1,
      max_ms: numberOf(stepItem, 'max_ms') ?? s.limits.step_ms_max,
    },
    run_until: runUntil,
    speed: {
      ...timeControl('speed', 'speed change'),
      min: numberOf(speedItem, 'min') ?? s.limits.speed_min,
      max: numberOf(speedItem, 'max') ?? s.limits.speed_max,
    },
    write: timeControl('write', 'write'),
  };
}

// --- the problem ----------------------------------------------------------------------------------

/** What `status.problem` chooses from besides the connection. */
export interface Candidate {
  /** Later is newer. */
  stamp: number;
  problem: Reason;
}

/** The connection's problem, if it has one (it beats every other). */
export function linkProblem(s: Snapshot): Reason | null {
  const link = linkOf(s);
  return link === 'rejected' || link === 'incompatible' || link === 'offline'
    ? linkReason(link, s)
    : null;
}

/** The newest of the candidates (null if none). */
export function newest(candidates: readonly Candidate[]): Reason | null {
  let best: Candidate | null = null;
  for (const c of candidates) if (!best || c.stamp > best.stamp) best = c;
  return best?.problem ?? null;
}

/** Structural equality of plain data (what the front's values are made of). */
export function sameData(a: unknown, b: unknown): boolean {
  if (Object.is(a, b)) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every((key) =>
    sameData((a as Record<string, unknown>)[key], (b as Record<string, unknown>)[key]),
  );
}
