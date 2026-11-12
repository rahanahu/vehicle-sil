import { CheckResult, LauncherEvent, RunResult, SessionSummary } from './protocol';
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
    start_paused: false,
    state: 'running',
    phase: null,
    stop: 'none',
    last_seq: null,
    last_t_ms: null,
    sim_ms: null,
    time_omitted_fields: [],
    time: null,
    last_command: null,
    check: null,
    participants: [],
    findings: [],
    result: null,
    launcher: { running: true, exit_code: null, signal: null, silent_s: null },
    log: {},
    problems: [],
    problems_omitted: 0,
    unreadable: [],
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
      control: null,
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
        failure: null,
        message: null,
        stopRequested: false,
        omittedFields: [],
        controlLink: null,
      },
      {
        name: 'B',
        state: 'disabled',
        required: true,
        pid: null,
        exitCode: null,
        signal: null,
        failure: null,
        message: null,
        stopRequested: false,
        omittedFields: [],
        controlLink: null,
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

  it('keeps how each participant is linked to its variables, from the events and from the result', () => {
    const session = applyEvents(
      emptySession(),
      events(
        {
          event: 'participant',
          name: 'A',
          state: 'joined',
          pid: null,
          exit_code: null,
          signal: null,
        },
        {
          event: 'control_link',
          name: 'A',
          link: { state: 'connected', variables: 17, version: null, late_messages: null },
        },
        {
          event: 'control_link',
          name: 'B',
          link: { state: 'no_hello', variables: null, version: null, late_messages: null },
        },
      ),
    );
    expect(session.participants.map((p) => [p.name, p.state, p.controlLink?.state])).toEqual([
      ['A', 'joined', 'connected'],
      ['B', null, 'no_hello'],
    ]);
    // 結果のほうが新しい（遅れて届いたメッセージの数を持つ）
    const ended = applyEvents(
      session,
      eventsFrom(3, [
        {
          event: 'result',
          result: {
            result: 'passed',
            findings: [],
            check: null,
            participants: [
              {
                name: 'A',
                required: true,
                enabled: true,
                ended: 'exited',
                exit_code: 0,
                signal: null,
                control_link: {
                  state: 'connected',
                  variables: 17,
                  version: null,
                  late_messages: 0,
                },
              },
              {
                name: 'C',
                required: true,
                enabled: true,
                ended: 'exited',
                exit_code: 0,
                signal: null,
                control_link: { state: 'none', variables: null, version: null, late_messages: 0 },
              },
            ],
          },
        },
      ]),
    );
    expect(ended.participants.find((p) => p.name === 'A')?.controlLink).toEqual({
      state: 'connected',
      variables: 17,
      version: null,
      late_messages: 0,
    });
    expect(ended.participants.find((p) => p.name === 'C')?.controlLink?.state).toBe('none');
  });

  it('keeps the last command and whether the launcher took it', () => {
    const session = applyEvents(
      emptySession(),
      events(
        { event: 'command', command: 'step', accepted: true, message: null, ms: 30 },
        {
          event: 'command',
          command: 'jump',
          accepted: false,
          message: 'unknown command',
          ms: null,
        },
      ),
    );
    expect(session.lastCommand).toEqual({
      name: 'jump',
      accepted: false,
      message: 'unknown command',
      ms: null,
      seq: 1,
    });
  });

  it('follows the time: from the events, and from a summary for a screen opened later', () => {
    const time = {
      event: 'time',
      sim_ms: 300,
      speed: 1,
      paused: true,
      actual_speed: null,
      late_steps: 1,
      max_late_ms: 200,
      slipped_ms: 200,
      late_ms: 0,
    };
    const session = applyEvents(emptySession(), events(time));
    expect(session.time).toEqual({
      simMs: 300,
      speed: 1,
      paused: true,
      actualSpeed: null,
      lateSteps: 1,
      maxLateMs: 200,
      slippedMs: 200,
      lateMs: 0,
    });
    const later = sessionFromSummary(
      summary({
        last_seq: 9,
        sim_ms: 300,
        time: { ...time, event: undefined } as unknown as SessionSummary['time'],
        start_paused: true,
        last_command: {
          seq: 8,
          command: 'pause',
          source: 'stdin',
          accepted: true,
          message: null,
          ms: null,
        },
      }),
    );
    expect(later.time).toEqual(session.time);
    expect(later.startPaused).toBe(true);
    expect(later.lastCommand).toEqual({
      name: 'pause',
      accepted: true,
      message: null,
      ms: null,
      seq: 8,
    });
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
      timing: null,
      participants: [
        {
          name: 'A',
          required: true,
          enabled: true,
          ended: 'exited',
          exit_code: 0,
          signal: null,
          left_processes: false,
          control_link: null,
        },
        {
          name: 'B',
          required: true,
          enabled: true,
          ended: 'exited',
          exit_code: 3,
          signal: null,
          left_processes: false,
          control_link: null,
        },
        {
          name: 'C',
          required: false,
          enabled: false,
          ended: 'not_started',
          exit_code: null,
          signal: null,
          left_processes: false,
          control_link: null,
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
        launcher: { running: false, exit_code: 70, signal: null, silent_s: null },
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
      reason: null,
      message: null,
      omitted_fields: [],
      stop_requested: false,
      control_link: null,
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
            reason: null,
            message: null,
            omitted_fields: ['command', 'environment'],
            stop_requested: false,
            control_link: null,
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

  it('keeps a participant that was asked to stop out of the failure colour, also from a later summary', () => {
    const asked = applyEvents(
      emptySession(),
      events({ event: 'participant', name: 'A', state: 'stop_requested', pid: 7 }),
    );
    const participant = {
      name: 'A',
      state: 'ended',
      pid: 7,
      exit_code: 130,
      signal: null,
      reason: null,
      message: null,
      omitted_fields: [],
      control_link: null,
    };
    // サーバーの印がなくても、手元で知っていれば残す
    const later = applySummary(
      asked,
      summary({ last_seq: 5, participants: [{ ...participant, stop_requested: false }] }),
      true,
    );
    expect(later.participants[0].stopRequested).toBe(true);
    // 再読み込み（手元に何もない）では、サーバーの印で
    const fresh = sessionFromSummary(
      summary({ last_seq: 5, participants: [{ ...participant, stop_requested: true }] }),
    );
    expect(fresh.participants[0].stopRequested).toBe(true);
  });

  it('does not take an older answer as running again, and keeps the notes of the newer state', () => {
    const ended = sessionFromSummary(
      summary({
        state: 'ended',
        last_seq: 9,
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        problems: [{ code: 'no-result', message: 'm' }],
      }),
    );
    const late = applySummary(ended, summary({ last_seq: 4, stop: 'requested' }));
    expect(late.launcher.running).toBe(false);
    expect(late.problems.map((p) => p.code)).toEqual(['no-result']);
  });

  it('takes only the launcher and the stop from an answer when the notifications bring the rest', () => {
    const session = applyEvents(emptySession(), events({ event: 'phase', phase: 'running' }));
    const finding = {
      severity: 'error',
      code: 'x',
      file: null,
      line: null,
      participant: null,
      network: null,
      message: 'm',
      detail: null,
    };
    const answered = applySummary(
      session,
      summary({ last_seq: 3, phase: 'stopping', stop: 'requested', findings: [finding] }),
      false,
      true,
    );
    expect(answered.findings).toEqual([]);
    expect(answered.phase).toBe('running');
    expect(answered.stopRequests).toBe(1);
    const next = applyEvents(answered, eventsFrom(1, [{ event: 'finding', ...finding }]));
    expect(next.findings).toHaveLength(1);
  });

  it('collects the parts it could not read, from summaries and events', () => {
    const session = sessionFromSummary(summary({ unreadable: ['result.participants'] }));
    const next = applyEvents(
      session,
      events({ event: 'log', source: 'A', line: 'x', unreadable: ['log.level'], t_ms: null }),
    );
    expect(next.unreadable).toEqual(['result.participants', 'log.level']);
    expect(next.elapsedMs).toBe(0);
    // 届いた時点の正の要約で置き換わる
    expect(applySummary(next, summary({ last_seq: 2 }), true).unreadable).toEqual([]);
  });

  it('counts the events whose seq is not after the last one', () => {
    const session = applyEvents(emptySession(), events(...logs('A', 3)));
    // 再送の重なりは数えない。再送でないのに戻った seq は数える
    expect(applyEvents(session, eventsFrom(1, logs('A', 2)))).toBe(session);
    const again = applyEvents(session, eventsFrom(1, logs('A', 2)), true);
    expect(again.staleEvents).toBe(2);
    expect(again.logs).toHaveLength(3);
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
      summary({
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
      }),
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
    // 印は行に数えない
    expect(dropped.logSources).toEqual({
      A: { lines: 2, dropped: 4 },
      B: { lines: 0, dropped: 1 },
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

  it('gives each mark its own key, also two marks at the same place', () => {
    const dropped = (session: ReturnType<typeof emptySession>) =>
      addDropped(session, {
        type: 'dropped',
        session: 1,
        reason: 'slow-client',
        log: { A: 2 },
        at_least: [],
        events: 0,
      });
    const twice = dropped(dropped(applyEvents(emptySession(), events(...logs('A', 1)))));
    const keys = twice.logs.map((line) => line.key);
    expect(new Set(keys).size).toBe(keys.length);
    expect(twice.logSources['A']).toEqual({ lines: 1, dropped: 4 });
  });

  it('drops the marks before the lines it drops over the limit, and does not count them as lines', () => {
    let session = applyEvents(emptySession(), events(...logs('A', 3)));
    session = addDropped(session, {
      type: 'dropped',
      session: 1,
      reason: 'slow-client',
      log: { A: 7 },
      at_least: [],
      events: 0,
    });
    session = applyEvents(session, eventsFrom(3, logs('A', MAX_LOG_LINES_PER_SOURCE - 3)));
    // 上限ちょうど（印は数えない）: 印は残る
    expect(session.logSources['A'].lines).toBe(MAX_LOG_LINES_PER_SOURCE);
    expect(session.logs.filter((l) => l.gap)).toHaveLength(1);
    session = applyEvents(session, eventsFrom(MAX_LOG_LINES_PER_SOURCE, logs('A', 4)));
    // 印をはさむ 4 行（seq 0 から 3）を落としたので、印も落ちる
    expect(session.logs.filter((l) => l.gap)).toHaveLength(0);
    expect(session.logs[0].seq).toBe(4);
    expect(session.logSources['A']).toEqual({ lines: MAX_LOG_LINES_PER_SOURCE, dropped: 11 });
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

  it('marks where the launcher could not write log lines, and counts them per source', () => {
    const session = applyEvents(
      emptySession(),
      events(
        ...logs('Engine', 2),
        { event: 'log_dropped', count: 125, sources: { Engine: 120, vehicle_sil_launch: 5 } },
        ...logs('Engine', 1),
      ),
    );
    const gaps = session.logs.filter((line) => line.gap);
    expect(gaps.map((g) => [g.source, g.seq, g.gap?.count, g.gap?.launcher])).toEqual([
      ['Engine', 2, 120, true],
      ['vehicle_sil_launch', 2, 5, true],
    ]);
    expect(session.logs.filter((l) => l.source === 'Engine').map((l) => l.seq)).toEqual([
      0, 1, 2, 3,
    ]);
    expect(session.logSources['Engine'].dropped).toBe(120);
    expect(session.logSources['vehicle_sil_launch'].dropped).toBe(5);
  });

  it('shows a participant as failed as soon as the launcher says so, and keeps it after it ends', () => {
    let session = applyEvents(
      emptySession(),
      events(
        { event: 'participant', name: 'Engine', state: 'started', pid: 7 },
        { event: 'participant', name: 'Engine', state: 'failed', reason: 'left_simulation' },
      ),
    );
    expect(session.participants[0].failure).toBe('left_simulation');
    session = applyEvents(
      session,
      eventsFrom(2, [
        { event: 'participant', name: 'Engine', state: 'ended', exit_code: 1, signal: null },
      ]),
    );
    expect(session.participants[0]).toMatchObject({
      state: 'ended',
      failure: 'left_simulation',
      exitCode: 1,
    });
    // つなぎ直した画面は、要約の reason から同じものを知る
    const again = sessionFromSummary(
      summary({
        last_seq: 2,
        participants: [
          {
            name: 'Engine',
            state: 'ended',
            pid: 7,
            exit_code: 1,
            signal: null,
            reason: 'left_simulation',
            message: null,
            omitted_fields: [],
            stop_requested: false,
            control_link: null,
          },
        ],
      }),
    );
    expect(again.participants[0].failure).toBe('left_simulation');
  });

  it('remembers that the launcher asked a participant to stop', () => {
    const session = applyEvents(
      emptySession(),
      events(
        { event: 'participant', name: 'Engine', state: 'stop_requested' },
        { event: 'participant', name: 'Engine', state: 'ended', exit_code: 130, signal: null },
        {
          event: 'participant',
          name: 'Logger',
          state: 'ended',
          exit_code: null,
          signal: 'SIGTERM',
        },
        {
          event: 'result',
          result: {
            result: 'failed',
            findings: [],
            participants: [
              {
                name: 'Engine',
                required: true,
                enabled: true,
                ended: 'stopped',
                exit_code: 130,
                signal: null,
              },
              {
                name: 'Logger',
                required: true,
                enabled: true,
                ended: 'stopped',
                exit_code: null,
                signal: 'SIGTERM',
              },
            ],
          },
        },
      ),
    );
    expect(session.participants.map((p) => p.stopRequested)).toEqual([true, true]);
  });

  it('knows when the launcher has been silent', () => {
    const session = sessionFromSummary(
      summary({ launcher: { running: true, exit_code: null, signal: null, silent_s: 10 } }),
    );
    expect(session.launcher.silentSeconds).toBe(10);
  });

  it('takes from the result of a summary that the launcher stopped a participant', () => {
    const result = {
      result: 'failed',
      findings: [],
      participants: [
        {
          name: 'Engine',
          required: true,
          enabled: true,
          ended: 'stopped',
          exit_code: 130,
          signal: null,
        },
      ],
    } as unknown as RunResult;
    const session = sessionFromSummary(
      summary({
        state: 'ended',
        last_seq: 9,
        result,
        participants: [
          {
            name: 'Engine',
            state: 'ended',
            pid: 7,
            exit_code: 130,
            signal: null,
            reason: null,
            message: null,
            omitted_fields: [],
            stop_requested: false,
            control_link: null,
          },
        ],
      }),
    );
    expect(session.participants[0]).toMatchObject({ state: 'ended', stopRequested: true });
  });
});
