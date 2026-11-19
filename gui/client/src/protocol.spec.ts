import { HttpError, errorMessage } from './http';
import { httpExamples, webSocketExamples } from './testing/protocol-examples';
import {
  ProtocolError,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readConfigText,
  readConnections,
  readDashboardResponse,
  readLauncherEvent,
  readRecording,
  readScenarioList,
  readServerMessage,
  readSessionResponse,
  readTimeResponse,
  readVariablesSummary,
  readWriteResponse,
} from './protocol';

/** Per request path, the function that reads the successful answer. */
const READERS: Record<string, (value: unknown) => unknown> = {
  'GET api/app': readAppInfo,
  'GET api/configs': readConfigList,
  'GET api/config': readConfigText,
  'POST api/check': readCheckResponse,
  'POST api/describe': readCheckResponse,
  'POST api/connections': readConnections,
  'POST api/run': readSessionResponse,
  'POST api/stop': readSessionResponse,
  'POST api/time': readTimeResponse,
  'GET api/session': readSessionResponse,
  'POST api/write': readWriteResponse,
  'GET api/dashboard': readDashboardResponse,
  'GET api/scenarios': readScenarioList,
  'GET api/recording': readRecording,
};

describe('the readers accept every example of gui/protocol', () => {
  it('reads the HTTP answers, and the message of every error', async () => {
    const examples = await httpExamples();
    expect(examples.length).toBeGreaterThan(10);
    for (const example of examples) {
      const key = `${example.request.method} ${example.request.path.split('?')[0]}`;
      if (example.status >= 400) {
        const error = HttpError.from(example.status, '', example.response);
        const message = (example.response as { error: { message: string } }).error.message;
        expect(errorMessage(error), example.name).toBe(message);
        continue;
      }
      const read = READERS[key];
      expect(read, `${example.name}: no reader for ${key}`).toBeDefined();
      expect(() => read(example.response), example.name).not.toThrow();
    }
  });

  it('sends the request bodies the server expects', async () => {
    const bodies = new Map(
      (await httpExamples())
        .filter((example) => example.status === 200 && example.request.method === 'POST')
        .map((example) => [example.name, Object.keys(example.request.body ?? {})]),
    );
    expect(bodies.get('check-passed')?.sort()).toEqual(['disable', 'file']);
    expect(bodies.get('run')?.sort()).toEqual(['disable', 'file']);
    expect(bodies.get('stop-force')).toEqual(['force']);
    expect(bodies.get('time')?.sort()).toEqual(['command', 'ms']);
  });

  it('reads the lines the server skipped in the standard error, and the fields it left out', async () => {
    const examples = await webSocketExamples();
    const stderr = examples
      .flatMap((example) => example.server)
      .map((message) => readServerMessage(message))
      .filter((message) => message?.type === 'error' && message.code === 'launcher-stderr');
    expect(stderr.length).toBeGreaterThan(0);
    expect(
      stderr.every((message) => message?.type === 'error' && message.skipped === undefined),
    ).toBe(true);
    // Notice of lines dropped in between (a shape the examples lack: a launcher writing more than 50 lines to stderr)
    const gap = readServerMessage({
      type: 'error',
      session: 1,
      code: 'launcher-stderr',
      message: 'the launcher wrote 7 more lines to its standard error here',
      detail: { skipped: 7 },
    });
    expect(gap).toMatchObject({ type: 'error', code: 'launcher-stderr', skipped: 7 });
    expect(
      readServerMessage({
        type: 'error',
        session: 1,
        code: 'x',
        message: 'm',
        detail: { skipped: -1 },
      }),
    ).not.toHaveProperty('skipped');

    const running = (await httpExamples()).find((example) => example.name === 'session-running');
    const raw = (running?.response as { session: Record<string, unknown> }).session;
    const participants = raw['participants'] as Record<string, unknown>[];
    expect(participants.length).toBeGreaterThan(0);
    const summary = readSessionResponse({
      session: {
        ...raw,
        time: { sim_ms: 250, omitted_fields: ['wall_clock'] },
        participants: [{ ...participants[0], omitted_fields: ['environment'] }],
      },
    });
    expect(summary?.time_omitted_fields).toEqual(['wall_clock']);
    expect(summary?.participants[0].omitted_fields).toEqual(['environment']);
    expect(readSessionResponse(running?.response)?.participants[0].omitted_fields).toEqual([]);
  });

  it('reads the launcher state and the stop request of a summary', async () => {
    const summary = readSessionResponse(
      (await httpExamples()).find((example) => example.name === 'run')?.response,
    );
    expect(summary?.launcher.running).toBe(true);
    expect(summary?.stop).toBe('none');
  });

  it('reads what the server and the launcher accept now', async () => {
    const examples = await webSocketExamples();
    const messages = examples.flatMap((example) => example.server.map((m) => readServerMessage(m)));
    // hello carries what the server accepts, also when there is no run
    const hello = messages.find((m) => m?.type === 'hello' && m.session === null);
    expect(hello?.type === 'hello' && hello.allowed?.start).toEqual({
      ok: true,
      reason: null,
      message: null,
    });
    // the allowed message carries the same shape, and its rev goes up with each change
    const own = examples
      .find((example) => example.name === 'ws-allowed')
      ?.server.map((m) => readServerMessage(m));
    const revs = (own ?? []).flatMap((m) =>
      m?.type === 'hello' || m?.type === 'allowed' ? [m.allowed?.rev ?? 0] : [],
    );
    expect(revs.length).toBe(3);
    expect(revs.every((rev, i) => i === 0 || rev > (revs[i - 1] ?? 0))).toBe(true);
    const states = messages.flatMap((m) =>
      m?.type === 'event' && m.event.event === 'state' ? [m.event] : [],
    );
    expect(states.length).toBeGreaterThan(0);
    for (const state of states) {
      expect(Object.keys(state.allowed)).toEqual(
        expect.arrayContaining(['pause', 'resume', 'step', 'speed', 'run_until', 'stop', 'write']),
      );
      expect(state.allowed['step']?.raw?.['max_ms'], 'the range stays in raw').toBeDefined();
    }
    // the summary keeps the last state with its seq, and says what the server accepts
    const summaries = messages.flatMap((m) => (m?.type === 'session' ? [m.session] : []));
    const kept = summaries.find((s) => s.launcher_state !== null);
    expect(kept?.launcher_state?.rev).toBeGreaterThan(0);
    expect(kept?.launcher_state?.seq).not.toBeNull();
    expect(summaries.every((s) => s.allowed !== null)).toBe(true);
    const running = summaries.find((s) => s.state === 'running');
    expect(running?.allowed?.start).toMatchObject({ ok: false, reason: 'already-running' });
    expect(running?.allowed?.start.message).toEqual(expect.any(String));
    const ended = summaries.find((s) => s.state === 'ended');
    expect(ended?.allowed?.start.ok).toBe(true);
    expect(ended?.allowed?.stop).toMatchObject({ ok: false, reason: 'not-running' });
  });

  it('reads the id and the reason of a command answer, and the id of a time command', async () => {
    const examples = await webSocketExamples();
    const commands = examples
      .flatMap((example) => example.server.map((m) => readServerMessage(m)))
      .flatMap((m) => (m?.type === 'event' && m.event.event === 'command' ? [m.event] : []));
    expect(
      commands.some((c) => c.reason !== null),
      'a refused command has its reason',
    ).toBe(true);
    expect(commands.every((c) => c.id === null || Number.isInteger(c.id))).toBe(true);
    const answer = (await httpExamples()).find((example) => example.name === 'time')?.response;
    expect(readTimeResponse(answer).id).toBeNull();
    expect(readTimeResponse({ session_id: 1, command: { command: 'pause', id: 7 } }).id).toBe(7);
  });

  it('reads the participants and states of an availability notification', async () => {
    const examples = await webSocketExamples();
    const told = examples
      .flatMap((example) => example.server.map((m) => readServerMessage(m)))
      .flatMap((m) => (m?.type === 'availability' ? [m] : []));
    expect(told.length).toBeGreaterThan(0);
    for (const m of told) {
      const states = Object.values(m.peers).flatMap((peer) => Object.values(peer));
      expect(states.length).toBeGreaterThan(0);
      expect(states.every((s) => ['unknown', 'active', 'inactive'].includes(s))).toBe(true);
    }
    expect(
      readServerMessage({
        type: 'availability',
        session: 3,
        peers: { Engine: { a: 'inactive', b: 'sometimes', c: 7 } },
      }),
    ).toEqual({
      type: 'availability',
      session: 3,
      peers: { Engine: { a: 'inactive', b: 'active', c: 'active' } },
    });
  });

  it('reads every WebSocket message, passing over what it does not know', async () => {
    const examples = await webSocketExamples();
    expect(examples.map((example) => example.name)).toContain('ws-pass');
    for (const example of examples) {
      for (const [i, message] of example.server.entries()) {
        const read = (() => {
          try {
            return readServerMessage(message);
          } catch (error) {
            throw new Error(`${example.name} server[${i}]: ${String(error)}`, { cause: error });
          }
        })();
        const raw = message as { type: string; event?: { event: string } };
        // A launcher event the parser does not know is skipped: a made-up one
        const skipped = raw.event?.event === 'mystery';
        if (raw.type === 'event' && skipped) {
          expect(read, `${example.name} server[${i}]`).toBeNull();
        } else {
          expect(read?.type, `${example.name} server[${i}]`).toBe(raw.type);
        }
      }
      for (const message of example.client) {
        // Fields of the messages the screen sends: the first message (AuthMessage) and variable watches (WatchMessage)
        const keys =
          (message as { type?: string }).type === 'auth'
            ? ['type', 'token', 'session', 'from_seq', 'session_without_check']
            : ['type', 'id', 'variables', 'period_ms'];
        expect(
          Object.keys(message as object).every((key) => keys.includes(key)),
          `${example.name}: ${JSON.stringify(message)}`,
        ).toBe(true);
      }
    }
  });

  it('reads the time, the commands and the timing of a run with time commands', async () => {
    const examples = await webSocketExamples();
    const read = (name: string) =>
      (examples.find((example) => example.name === name)?.server ?? []).map((message) =>
        readServerMessage(message),
      );
    const time = read('ws-time');
    const events = time.flatMap((m) => (m?.type === 'event' ? [m.event] : []));
    expect(events.filter((e) => e.event === 'command').map((e) => e.unreadable)).not.toContain(
      expect.anything(),
    );
    const step = events.find((e) => e.event === 'command' && e.command === 'step');
    expect(step).toMatchObject({ accepted: true, ms: 30, message: null });
    const refused = events.filter((e) => e.event === 'command' && !e.accepted);
    expect(refused.length).toBe(1);
    const times = events.flatMap((e) => (e.event === 'time' ? [e] : []));
    expect(times[0]).toMatchObject({ sim_ms: 0, late_steps: 0 });
    // Whether it stands still and the speed set are the state's to say
    expect(times.some((t) => 'paused' in t || 'speed' in t)).toBe(false);
    const states = events.flatMap((e) => (e.event === 'state' ? [e] : []));
    expect(states.some((s) => s.speed === 1 && s.paused === false)).toBe(true);
    const end = time.at(-1);
    expect(end?.type === 'session' && end.session.result?.timing).toMatchObject({
      speed: 1,
      time_step_ms: 10,
    });
    expect(end?.type === 'session' && end.session.launcher_state?.paused).toBe(false);

    const late = read('ws-late').flatMap((m) => (m?.type === 'event' ? [m.event] : []));
    expect(late.find((e) => e.event === 'time' && (e.late_ms ?? 0) > 0) as unknown).toMatchObject({
      late_ms: 200,
      late_steps: 1,
      slipped_ms: 200,
    });
    expect(late.some((e) => e.event === 'finding' && e.finding.code === 'behind-real-time')).toBe(
      true,
    );
  });

  it('reads the control links of the participants, in the check, the events and the summary', async () => {
    const example = (await webSocketExamples()).find((e) => e.name === 'ws-control-trouble');
    const read = (example?.server ?? []).map((m) => readServerMessage(m));
    const links = read.flatMap((m) =>
      m?.type === 'event' && m.event.event === 'control_link' ? [[m.event.name, m.event.link]] : [],
    );
    const link = (state: string, extra = {}) => ({
      state,
      variables: null,
      version: null,
      late_messages: null,
      ...extra,
    });
    expect(links).toEqual([
      ['Engine', link('no_hello')],
      ['Dashboard', link('version_mismatch', { version: 1 })],
      ['Gauge', link('not_offered')],
      ['Logger', link('none')],
    ]);
    const check = read.flatMap((m) =>
      m?.type === 'event' && m.event.event === 'check' ? [m.event.check] : [],
    )[0];
    expect(check?.participants.map((p) => p.control)).toEqual([
      { transport: 'silkit', wire_version: 2, wire_revision: 1 },
      { transport: 'silkit', wire_version: 2, wire_revision: 1 },
      null,
      null,
    ]);
    expect(check?.findings.map((f) => f.code)).toContain('control-link-not-offered');
    const end = read.at(-1);
    const states = ['no_hello', 'version_mismatch', 'not_offered', 'none'];
    expect(
      end?.type === 'session' && end.session.participants.map((p) => p.control_link?.state),
    ).toEqual(states);
    expect(
      end?.type === 'session' && end.session.result?.participants.map((p) => p.control_link?.state),
    ).toEqual(states);
    expect(
      end?.type === 'session' && end.session.result?.participants[0].control_link?.late_messages,
    ).toBe(0);
  });

  it('reads a time event that has only sim_ms', () => {
    const read = readLauncherEvent({ event: 'time', seq: 3, t_ms: 0, sim_ms: 1000 }, 'e', true);
    expect(read).toMatchObject({ sim_ms: 1000, actual_speed: null, late_steps: null });
    expect(read?.unreadable).toBeUndefined();
  });

  it('reads the summary in the hello of a replay', async () => {
    const replay = (await webSocketExamples()).find((example) => example.name === 'ws-replay');
    const hello = readServerMessage(replay?.server[0]);
    expect(hello?.type).toBe('hello');
    if (hello?.type !== 'hello') return;
    expect(hello.session?.state).toBe('ended');
    expect(hello.session?.result?.result).toBe('passed');
    expect(hello.replay?.from_seq).toBeNull();
  });
});

describe('the readers read summaries and events part by part', () => {
  it('keeps what it can read of a summary, and names the parts it cannot read', async () => {
    const ended = (await httpExamples()).find((example) => example.name === 'session-ended');
    const raw = structuredClone((ended?.response as { session: Record<string, unknown> }).session);
    raw['findings'] = [{ severity: 'error', message: 'm', code: 5 }, 'junk'];
    raw['participants'] = [{ name: 'A', state: 'ended', signal: 9 }];
    (raw['result'] as Record<string, unknown>)['vehicle_sil_run'] = 3;
    const summary = readSessionResponse({ session: raw });
    expect(summary?.findings).toEqual([expect.objectContaining({ code: '', message: 'm' })]);
    expect(summary?.participants[0]).toMatchObject({ name: 'A', state: 'ended', signal: null });
    expect(summary?.result?.result).toBe('passed');
    expect([...(summary?.unreadable ?? [])].sort()).toEqual([
      'findings',
      'findings.code',
      'participants.signal',
      'result.vehicle_sil_run',
    ]);
    // If the core (id, state, launcher, stop) cannot be read, the summary cannot be read
    expect(() => readSessionResponse({ session: { ...raw, launcher: null } })).toThrow(
      /response\.session\.launcher: expected an object/,
    );
  });

  it('reads an event whose loose fields are missing or of other types, and names them', () => {
    const read = readServerMessage({
      type: 'event',
      session: 1,
      event: { event: 'participant', seq: 4, name: 'A', state: 'joined', signal: 9 },
    });
    expect(read).toMatchObject({
      type: 'event',
      event: { name: 'A', state: 'joined', signal: null, t_ms: null },
    });
    expect(read?.type === 'event' && read.event.unreadable).toEqual(['participant.signal']);
  });
});

describe('the readers refuse what they cannot read', () => {
  const finding = {
    event: 'finding',
    seq: 3,
    t_ms: 30,
    severity: 'error',
    code: 'x',
    file: null,
    line: null,
    participant: null,
    network: null,
    message: 'm',
    detail: null,
  };

  it('ignores unknown fields and unknown events', () => {
    expect(readLauncherEvent({ ...finding, colour: 'blue' }, 'e')).toMatchObject({
      event: 'finding',
      finding: { code: 'x', message: 'm' },
    });
    expect(readLauncherEvent({ event: 'mystery', seq: 1, t_ms: 0 }, 'e')).toBeNull();
    expect(readServerMessage({ type: 'later', anything: 1 })).toBeNull();
  });

  it('reads a run with a scenario: its steps, the check, the result and the summary', async () => {
    const run = (await webSocketExamples()).find((example) => example.name === 'ws-scenario');
    expect(run).toBeDefined();
    const messages = (run?.server ?? []).map((m) => readServerMessage(m));
    const steps = messages.flatMap((m) =>
      m?.type === 'event' && m.event.event === 'scenario_step' ? [m.event.step] : [],
    );
    expect(steps.map((step) => `${step.index} ${step.status}`)).toEqual([
      '1 running',
      '1 passed',
      '2 running',
      '2 failed',
    ]);
    expect(steps[3]).toMatchObject({ line: 7, code: 'scenario-failed', kind: 'expect' });
    const ended = messages.filter((m) => m?.type === 'session').at(-1);
    expect(ended?.type === 'session' && ended.session?.scenario).toMatchObject({
      file: 'scenarios/fails.yaml',
      name: 'speed too high',
      step_count: 3,
      status: 'failed',
    });
    const result = messages.find((m) => m?.type === 'event' && m.event.event === 'result');
    expect(
      result?.type === 'event' && result.event.event === 'result' && result.event.result.scenario,
    ).toMatchObject({ status: 'failed', steps: [{}, {}, { status: 'not_run' }] });
    // A step whose index or status cannot be read is not a step
    expect(() =>
      readLauncherEvent({ event: 'scenario_step', seq: 7, t_ms: 0, index: 'x', status: 'ok' }, 'e'),
    ).toThrow(ProtocolError);
  });

  it('names the place of a missing or wrong field', () => {
    expect(() => readLauncherEvent({ ...finding, message: 5 }, 'e')).toThrow(
      new ProtocolError('e.message', 'a string'),
    );
    expect(() => readLauncherEvent({ event: 'log', seq: 1, t_ms: 0, source: 'A' }, 'e')).toThrow(
      /e\.line: expected a string/,
    );
  });

  it('refuses a format version it does not know', () => {
    expect(() => readServerMessage({ type: 'hello', vehicle_sil_gui: 2 })).toThrow(
      /vehicle_sil_gui: expected format version 1/,
    );
  });

  it('falls back to the status when an error is not in the server form', () => {
    const error = HttpError.from(502, 'Bad Gateway', 'down');
    expect(errorMessage(error)).toBe('HTTP 502 Bad Gateway');
    expect(error.problem).toBeNull();
    expect(errorMessage(new HttpError(0, '', null))).toBe('HTTP 0');
  });

  it('reads the variables of every summary that has them', async () => {
    const summaries = (await webSocketExamples())
      .flatMap((example) => example.server)
      .flatMap((message) => {
        const m = message as {
          type: string;
          session?: { variables?: unknown };
          variables?: unknown;
        };
        return m.type === 'hello' && m.session
          ? [m.session.variables]
          : m.type === 'variables'
            ? [m.variables]
            : [];
      });
    const read = summaries.map((summary) => readVariablesSummary(summary));
    expect(read.length).toBeGreaterThan(10);
    expect(new Set(read.map((v) => v.state))).toEqual(
      new Set(['off', 'connecting', 'ready', 'failed', 'ended']),
    );
    const ready = read.find((v) => v.state === 'ready');
    const variable = (name: string) => ready?.peers[0].variables.find((v) => v.name === name);
    expect(variable('ecu.connected')).toMatchObject({ type: 'bool', writable: true });
    expect(variable('engine.demand')).toMatchObject({
      name: 'engine.demand',
      type: 'real',
      writable: true,
    });
    expect(variable('engine.mode')?.enum).toEqual([
      { name: 'off', value: 0 },
      { name: 'on', value: 1 },
    ]);
  });
});
