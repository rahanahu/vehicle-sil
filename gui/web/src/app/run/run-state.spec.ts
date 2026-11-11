import { CheckResult, LauncherEvent, RunResult, SessionSummary } from '../core/api';
import { MAX_LOG_LINES, applyEvents, applySummary, isEnded, sessionFromSummary } from './run-state';

function summary(extra: Partial<SessionSummary> = {}): SessionSummary {
  return {
    id: 1,
    file: 'sim.yaml',
    disable: [],
    speed: null,
    state: 'running',
    phase: null,
    stop_requests: 0,
    last_seq: null,
    sim_ms: null,
    participants: [],
    findings: [],
    result: null,
    problems: [],
    ...extra,
  };
}

const emptySession = () => sessionFromSummary(summary(), null);

/** 試験の事象を作る。seq は渡した順に 0 から。 */
function events(...items: Record<string, unknown>[]): LauncherEvent[] {
  return items.map((item, seq) => ({ seq, t_ms: seq * 10, ...item }) as unknown as LauncherEvent);
}

function check(participants: { name: string; enabled?: boolean; required?: boolean }[]) {
  return {
    file: 'sim.yaml',
    result: 'passed',
    errors: 0,
    warnings: 0,
    notes: 0,
    stop_at_ms: 1000,
    participants: participants.map((p) => ({
      name: p.name,
      line: 1,
      required: p.required ?? true,
      enabled: p.enabled ?? true,
      step_ms: 10,
      status: p.enabled === false ? 'disabled' : 'described',
      variables: null,
    })),
    networks: [],
    findings: [],
  } satisfies CheckResult;
}

const finding = {
  severity: 'error',
  code: 'participant-failed',
  file: 'sim.yaml',
  line: 3,
  participant: 'B',
  network: null,
  message: "participant 'B' failed",
  detail: null,
};

describe('applyEvents', () => {
  it('follows the phases, the participants and the time', () => {
    const session = applyEvents(
      emptySession(),
      events(
        {
          event: 'hello',
          vehicle_sil_events: 1,
          vehicle_sil: '1',
          pid: 1,
          file: 'sim.yaml',
          arguments: [],
        },
        { event: 'phase', phase: 'checking' },
        { event: 'check', check: check([{ name: 'A' }, { name: 'B', enabled: false }]) },
        { event: 'phase', phase: 'starting' },
        {
          event: 'participant',
          name: 'A',
          state: 'started',
          pid: 42,
          exit_code: null,
          signal: null,
        },
        { event: 'phase', phase: 'running' },
        {
          event: 'participant',
          name: 'A',
          state: 'joined',
          pid: null,
          exit_code: null,
          signal: null,
        },
        { event: 'time', sim_ms: 300 },
      ),
    );
    expect(session.file).toBe('sim.yaml');
    expect(session.phase).toBe('running');
    expect(session.lastSeq).toBe(7);
    expect(session.elapsedMs).toBe(70);
    expect(session.simMs).toBe(300);
    expect(session.participants).toEqual([
      { name: 'A', state: 'joined', required: true, pid: 42, exitCode: null, signal: null },
      { name: 'B', state: 'disabled', required: true, pid: null, exitCode: null, signal: null },
    ]);
    expect(isEnded(session)).toBe(false);
  });

  it('ignores events it already has (a replay that overlaps)', () => {
    const all = events(
      { event: 'log', source: 'A', line: 'one' },
      { event: 'log', source: 'A', line: 'two' },
      { event: 'log', source: 'B', line: 'three' },
    );
    const first = applyEvents(emptySession(), all.slice(0, 2));
    const again = applyEvents(first, all);
    expect(again.logs.map((l) => l.line)).toEqual(['one', 'two', 'three']);
    expect(applyEvents(again, all.slice(0, 2))).toBe(again);
  });

  it('keeps the newest log lines and counts the dropped ones', () => {
    const many = Array.from({ length: MAX_LOG_LINES + 5 }, (_, i) => ({
      event: 'log',
      source: 'A',
      line: `line ${i}`,
    }));
    const session = applyEvents(emptySession(), events(...many));
    expect(session.logs).toHaveLength(MAX_LOG_LINES);
    expect(session.logs[0].line).toBe('line 5');
    expect(session.droppedLogLines).toBe(5);
  });

  it('keeps the reason of a refused command', () => {
    const session = applyEvents(
      emptySession(),
      events(
        { event: 'command', command: 'stop', accepted: true, message: null },
        { event: 'command', command: 'jump', accepted: false, message: 'unknown command' },
      ),
    );
    expect(session.rejectedCommand).toBe('unknown command');
  });

  it('ends with the result, which alone tells how the participants ended', () => {
    const result = {
      file: 'sim.yaml',
      result: 'failed',
      errors: 1,
      warnings: 0,
      notes: 0,
      simulation: 'aborted',
      interrupted_by: null,
      participants: [
        {
          name: 'A',
          required: true,
          enabled: true,
          ended: 'exited',
          exit_code: 0,
          signal: null,
          left_processes: false,
        },
        {
          name: 'B',
          required: true,
          enabled: true,
          ended: 'exited',
          exit_code: 3,
          signal: null,
          left_processes: false,
        },
        {
          name: 'C',
          required: false,
          enabled: false,
          ended: 'not_started',
          exit_code: null,
          signal: null,
          left_processes: false,
        },
      ],
      findings: [finding],
      check: check([{ name: 'A' }, { name: 'B' }, { name: 'C', enabled: false }]),
    } satisfies RunResult;
    const session = applyEvents(
      emptySession(),
      events({ event: 'finding', finding }, { event: 'result', result }),
    );
    expect(isEnded(session)).toBe(true);
    expect(session.phase).toBe('ended');
    expect(session.findings).toEqual([finding]);
    expect(session.check).toBe(result.check);
    expect(session.participants.map((p) => [p.name, p.state, p.exitCode])).toEqual([
      ['A', 'ended', 0],
      ['B', 'ended', 3],
      ['C', 'disabled', null],
    ]);
  });

  it('passes over events it does not know', () => {
    const session = applyEvents(
      emptySession(),
      events({ event: 'registry', uri: 'silkit://x' }, { event: 'phase', phase: 'paused' }),
    );
    expect(session.phase).toBe('paused');
    expect(session.lastSeq).toBe(1);
  });

  it('does not go back to an earlier phase when a replay follows the summary', () => {
    const session = sessionFromSummary(summary({ phase: 'running', last_seq: 9 }), null);
    const replayed = applyEvents(session, events({ event: 'phase', phase: 'checking' }));
    expect(replayed.phase).toBe('running');
  });
});

describe('applySummary', () => {
  it('ends the session when the server says so, even without a result', () => {
    const session = applyEvents(emptySession(), events({ event: 'phase', phase: 'running' }));
    expect(isEnded(session)).toBe(false);
    const ended = applySummary(
      session,
      summary({
        state: 'ended',
        phase: 'running',
        last_seq: 0,
        stop_requests: 1,
        problems: [{ code: 'no-result', message: 'no result' }],
      }),
    );
    expect(isEnded(ended)).toBe(true);
    expect(ended.stopRequests).toBe(1);
    expect(ended.problems.map((p) => p.code)).toEqual(['no-result']);
  });

  it('takes the participants from a summary newer than the events it has', () => {
    const participant = {
      name: 'A',
      required: true,
      enabled: true,
      state: 'joined',
      pid: 7,
      exit_code: null,
      signal: null,
    };
    const session = applySummary(
      emptySession(),
      summary({ last_seq: 12, participants: [participant] }),
    );
    expect(session.participants.map((p) => [p.name, p.state])).toEqual([['A', 'joined']]);
  });
});
