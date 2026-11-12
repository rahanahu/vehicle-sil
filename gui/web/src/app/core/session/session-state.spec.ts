import { CheckResult, LauncherEvent, RunResult, SessionSummary } from '../api';
import {
  MAX_LOG_LINES,
  MAX_LOG_LINES_PER_SOURCE,
  MAX_PROBLEMS,
  addProblem,
  applyEvents,
  applySummary,
  addDropped,
  runOutcome,
  sessionFromSummary,
} from './session-state';

function summary(extra: Partial<SessionSummary> = {}): SessionSummary {
  return {
    id: 1,
    file: 'sim.yaml',
    disable: [],
    speed: null,
    state: 'running',
    phase: null,
    stop: 'none',
    last_seq: null,
    last_t_ms: null,
    sim_ms: null,
    time_omitted_fields: [],
    check: null,
    participants: [],
    findings: [],
    result: null,
    launcher: { running: true, exit_code: null, signal: null },
    log: {},
    problems: [],
    problems_omitted: 0,
    ...extra,
  };
}

const emptySession = () => sessionFromSummary(summary());

/** 試験の事象を作る。seq は渡した順に from から。 */
function eventsFrom(from: number, items: Record<string, unknown>[]): LauncherEvent[] {
  return items.map(
    (item, i) => ({ seq: from + i, t_ms: (from + i) * 10, ...item }) as unknown as LauncherEvent,
  );
}

function events(...items: Record<string, unknown>[]): LauncherEvent[] {
  return eventsFrom(0, items);
}

function logs(source: string, count: number): Record<string, unknown>[] {
  return Array.from({ length: count }, (_, i) => ({
    event: 'log',
    source,
    line: `${source} ${i}`,
  }));
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
      {
        name: 'A',
        state: 'joined',
        required: true,
        pid: 42,
        exitCode: null,
        signal: null,
        omittedFields: [],
      },
      {
        name: 'B',
        state: 'disabled',
        required: true,
        pid: null,
        exitCode: null,
        signal: null,
        omittedFields: [],
      },
    ]);
    expect(runOutcome(session)).toBe('running');
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

  it('keeps the newest lines of each source, and the lines of a quiet source', () => {
    const quiet = applyEvents(emptySession(), events(...logs('Quiet', 3)));
    const loud = applyEvents(quiet, eventsFrom(3, logs('Loud', MAX_LOG_LINES_PER_SOURCE + 5)));
    expect(loud.logs.filter((l) => l.source === 'Quiet').map((l) => l.line)).toEqual([
      'Quiet 0',
      'Quiet 1',
      'Quiet 2',
    ]);
    const kept = loud.logs.filter((l) => l.source === 'Loud');
    expect(kept).toHaveLength(MAX_LOG_LINES_PER_SOURCE);
    expect(kept[0].line).toBe('Loud 5');
    expect(loud.logSources).toEqual({
      Quiet: { lines: 3, dropped: 0 },
      Loud: { lines: MAX_LOG_LINES_PER_SOURCE, dropped: 5 },
    });
  });

  it('over the whole limit, drops from the source that has the most lines', () => {
    const sources = Math.ceil(MAX_LOG_LINES / MAX_LOG_LINES_PER_SOURCE) + 1;
    let session = emptySession();
    let seq = 0;
    for (let n = 0; n < sources; n++) {
      // 最後の出どころは少しだけ
      const count = n === sources - 1 ? 10 : MAX_LOG_LINES_PER_SOURCE;
      session = applyEvents(session, eventsFrom(seq, logs(`S${n}`, count)));
      seq += count;
    }
    expect(session.logs).toHaveLength(MAX_LOG_LINES);
    expect(session.logSources[`S${sources - 1}`]).toEqual({ lines: 10, dropped: 0 });
    const dropped = Object.values(session.logSources).reduce((sum, s) => sum + s.dropped, 0);
    expect(dropped).toBe(seq - MAX_LOG_LINES);
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

  it('does not go back after a phase it does not know', () => {
    const session = applyEvents(
      emptySession(),
      events(
        { event: 'phase', phase: 'running' },
        { event: 'phase', phase: 'paused' },
        { event: 'phase', phase: 'checking' },
      ),
    );
    expect(session.phase).toBe('paused');
    expect(applyEvents(session, eventsFrom(3, [{ event: 'phase', phase: 'stopping' }])).phase).toBe(
      'stopping',
    );
  });

  it('does not go back to an earlier phase when a replay follows the summary', () => {
    const session = sessionFromSummary(summary({ phase: 'running', last_seq: 9 }));
    const replayed = applyEvents(session, events({ event: 'phase', phase: 'checking' }));
    expect(replayed.phase).toBe('running');
  });
});

describe('applySummary', () => {
  it('ends the session when the server says so, even without a result', () => {
    const session = applyEvents(emptySession(), events({ event: 'phase', phase: 'running' }));
    expect(runOutcome(session)).toBe('running');
    const ended = applySummary(
      session,
      summary({
        state: 'ended',
        phase: 'running',
        last_seq: 0,
        stop: 'requested',
        launcher: { running: false, exit_code: 70, signal: null },
        problems: [{ code: 'no-result', message: 'no result' }],
      }),
    );
    expect(runOutcome(ended)).toBe('crashed');
    expect(ended.launcher.exitCode).toBe(70);
    expect(ended.stopRequests).toBe(1);
    expect(ended.problems.map((p) => p.code)).toEqual(['no-result']);
  });

  it('takes the participants from a summary newer than the events it has, with the check', () => {
    const participant = {
      name: 'A',
      state: 'joined',
      pid: 7,
      exit_code: null,
      signal: null,
      omitted_fields: [],
    };
    const session = applySummary(
      emptySession(),
      summary({
        last_seq: 12,
        participants: [participant],
        check: check([
          { name: 'B', enabled: false },
          { name: 'A', required: false },
        ]),
      }),
    );
    expect(session.participants.map((p) => [p.name, p.state, p.required])).toEqual([
      ['B', 'disabled', true],
      ['A', 'joined', false],
    ]);
  });

  it('keeps an older summary from an answer from taking back the phase', () => {
    const session = applyEvents(
      emptySession(),
      events({ event: 'phase', phase: 'running' }, { event: 'phase', phase: 'stopping' }),
    );
    const answered = applySummary(
      session,
      summary({ phase: 'running', last_seq: 0, stop: 'requested' }),
    );
    expect(answered.phase).toBe('stopping');
    expect(answered.stopRequests).toBe(1);
  });

  it('keeps the names of the fields the server left out, until an event brings them', () => {
    const session = sessionFromSummary(
      summary({
        last_seq: 4,
        sim_ms: 300,
        time_omitted_fields: ['wall_clock'],
        participants: [
          {
            name: 'A',
            state: 'joined',
            pid: 7,
            exit_code: null,
            signal: null,
            omitted_fields: ['command', 'environment'],
          },
        ],
      }),
    );
    expect(session.participants[0].omittedFields).toEqual(['command', 'environment']);
    expect(session.timeOmittedFields).toEqual(['wall_clock']);

    const later = applyEvents(
      session,
      eventsFrom(5, [
        { event: 'participant', name: 'A', state: 'ended', pid: 7, exit_code: 0, signal: null },
        { event: 'time', sim_ms: 400 },
      ]),
    );
    expect(later.participants[0].omittedFields).toEqual([]);
    expect(later.timeOmittedFields).toEqual([]);
  });

  it('lists the log sources of the summary, even without lines', () => {
    const session = sessionFromSummary(summary({ log: { Engine: { kept: 0, dropped: 4 } } }));
    expect(session.logSources).toEqual({ Engine: { lines: 0, dropped: 0 } });
  });
});

describe('runOutcome', () => {
  it('can still be stopped after the result while the launcher runs', () => {
    const result = { result: 'passed', findings: [], participants: [] } as unknown as RunResult;
    const session = applyEvents(emptySession(), events({ event: 'result', result }));
    expect(session.result).toBe(result);
    expect(session.launcher.running).toBe(true);
    expect(runOutcome(session)).toBe('finishing');
    const ended = applySummary(
      session,
      summary({ state: 'ended', launcher: { running: false, exit_code: 0, signal: null } }),
    );
    expect(runOutcome(ended)).toBe('ended');
  });

  it('tells stopping from stopping without waiting', () => {
    expect(runOutcome(applySummary(emptySession(), summary({ stop: 'requested' })))).toBe(
      'stopping',
    );
    expect(runOutcome(applySummary(emptySession(), summary({ stop: 'forced' })))).toBe('forcing');
  });
});

describe('addDropped', () => {
  it('counts the lines that were not sent, and marks the place in the log', () => {
    const session = applyEvents(emptySession(), events(...logs('A', 2)));
    const dropped = addDropped(session, {
      type: 'dropped',
      session: 1,
      reason: 'not-kept',
      log: { A: 4, B: 1 },
      at_least: ['B'],
      events: 0,
    });
    expect(dropped.logSources).toEqual({
      A: { lines: 3, dropped: 4 },
      B: { lines: 1, dropped: 1 },
    });
    const next = applyEvents(dropped, eventsFrom(2, logs('A', 1)));
    expect(next.logs.map((l) => (l.gap ? `gap ${l.source} ${l.gap.count}` : l.line))).toEqual([
      'A 0',
      'A 1',
      'gap A 4',
      'gap B 1',
      'A 0',
    ]);
    expect(next.logs.find((l) => l.source === 'B')?.gap?.atLeast).toBe(true);
  });
});

describe('addProblem', () => {
  it('counts the notes over the limit instead of keeping them', () => {
    let session = emptySession();
    for (let i = 0; i < MAX_PROBLEMS + 3; i++) {
      session = addProblem(session, { code: 'launcher-output', message: `line ${i}` });
    }
    expect(session.problems).toHaveLength(MAX_PROBLEMS);
    expect(session.problemsOmitted).toBe(3);
  });
});
