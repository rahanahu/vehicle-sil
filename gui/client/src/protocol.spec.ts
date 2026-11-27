import { createClient } from './client';
import { HttpError, HttpRequest, HttpTransport, errorMessage } from './http';
import { httpExamples, protocolSchemas, webSocketExamples } from './testing/protocol-examples';
import { FakeWebSockets } from './testing/fake-web-socket';
import {
  ProtocolError,
  Unreadable,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readConfigText,
  readConnections,
  readDashboardResponse,
  readLauncherEvent,
  readOperationReport,
  readPrepareResponse,
  readRecording,
  readRunResponse,
  readRuntimeStatus,
  readSavedConfig,
  readScenarioList,
  readServerAllowed,
  readServerMessage,
  readSessionResponse,
  readSessionRuntime,
  readTimeResponse,
  readVariablesReport,
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
  'POST api/config': readSavedConfig,
  'POST api/connections': readConnections,
  'POST api/prepare': readPrepareResponse,
  'POST api/run': readRunResponse,
  'POST api/stop-session': readRunResponse,
  'POST api/time': readTimeResponse,
  'GET api/session': readSessionResponse,
  'POST api/write': readWriteResponse,
  'GET api/dashboard': readDashboardResponse,
  'GET api/scenarios': readScenarioList,
  'GET api/recording': readRecording,
};

describe('the readers accept every example of gui/protocol', () => {
  it('keeps the simulation marker when the configuration listing sends it', () => {
    const files = [
      { path: 'simulation.yaml', simulation: true },
      { path: 'scenarios/drive.yaml', simulation: false },
      { path: 'older.yaml' },
    ];
    expect(readConfigList({ files, truncated: false }).files).toEqual(files);
  });

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
      if (key.startsWith('GET api/timeline/')) {
        expect((example.response as Record<string, unknown>)['vehicle_sil_timeline']).toBe(1);
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
    expect(bodies.get('stop-force')?.sort()).toEqual(['force', 'instance', 'run_id']);
    expect(bodies.get('time')?.sort()).toEqual(['command', 'instance', 'ms', 'run_id']);
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
      run_id: '1',
      code: 'launcher-stderr',
      message: 'the launcher wrote 7 more lines to its standard error here',
      detail: { skipped: 7 },
    });
    expect(gap).toMatchObject({ type: 'error', code: 'launcher-stderr', skipped: 7 });
    expect(
      readServerMessage({
        type: 'error',
        run_id: '1',
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

  it('reads the launcher state and the stop request of a summary, and the run acknowledgement', async () => {
    const ack = readRunResponse(
      (await httpExamples()).find((example) => example.name === 'run')?.response,
    );
    expect(ack).toEqual({ instance: '0123456789abcdef', run_id: '1' });
    const summary = readSessionResponse(
      (await httpExamples()).find((example) => example.name === 'session-running')?.response,
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
    // and whether it saves a configuration file (save has a caution besides)
    expect(hello?.type === 'hello' && hello.allowed?.save).toEqual({
      ok: true,
      reason: null,
      message: null,
      caution: null,
    });
    // the allowed message carries the same shape, ordered by stream_seq
    const own = examples
      .find((example) => example.name === 'ws-allowed')
      ?.server.map((m) => readServerMessage(m));
    const seqs = (own ?? []).flatMap((m) =>
      m?.type === 'hello' || m?.type === 'allowed' ? [m.stream_seq ?? 0] : [],
    );
    expect(seqs.length).toBe(3);
    expect(seqs.every((seq, i) => i === 0 || seq > (seqs[i - 1] ?? 0))).toBe(true);
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
    expect(readTimeResponse({ run_id: '1', command: { command: 'pause', id: 7 } }).id).toBe(7);
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
        run_id: '3',
        peers: { Engine: { a: 'inactive', b: 'sometimes', c: 7 } },
      }),
    ).toEqual({
      type: 'availability',
      run_id: '3',
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
            ? ['type', 'token', 'instance', 'run_id', 'after_stream_seq']
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
    expect(step).toMatchObject({ accepted: true, ms: 25, message: null });
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
      time_step_ms: 5,
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
    expect(hello.replay?.after_stream_seq).toBeNull();
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
      run_id: '1',
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

  it('refuses an allowed without save, or a save without caution', () => {
    const item = { ok: true, reason: null, message: null };
    const allowed = { rev: 1, start: item, stop: item, force: item };
    expect(() =>
      readServerAllowed({ ...allowed, save: { ...item, caution: null } }, 'a'),
    ).not.toThrow();
    expect(() => readServerAllowed(allowed, 'a')).toThrow(ProtocolError);
    expect(() => readServerAllowed({ ...allowed, save: item }, 'a')).toThrow(ProtocolError);
  });

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
    expect(() => readServerMessage({ type: 'hello', vehicle_sil_gui: 99 })).toThrow(
      /vehicle_sil_gui: expected format version 4/,
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
    expect(variable('ecu.connected')).toMatchObject({
      type: 'bool',
      writable: true,
      builtin: true,
    });
    expect(variable('engine.demand')).toMatchObject({
      name: 'engine.demand',
      type: 'real',
      writable: true,
      builtin: false,
    });
    expect(variable('engine.mode')?.enum).toEqual([
      { name: 'off', value: 0 },
      { name: 'on', value: 1 },
    ]);
  });

  it('reads additive runtime status, prepare responses, and 2-axis launcher fields', () => {
    const rt = readRuntimeStatus(
      {
        mode: 'hot',
        state: 'ready',
        config: 'a.yaml',
        reason: null,
        message: null,
      },
      'runtime',
    );
    expect(rt).toEqual({
      mode: 'hot',
      state: 'ready',
      config: 'a.yaml',
      reason: null,
      message: null,
    });

    const prep = readPrepareResponse({
      instance: 'inst-1',
      runtime: {
        mode: 'cold',
        state: 'cold',
        config: 'f.yaml',
        reason: 'fixed-registry',
        message: 'fixed',
      },
    });
    expect(prep).toMatchObject({
      instance: 'inst-1',
      runtime: { mode: 'cold', reason: 'fixed-registry' },
    });

    expect(
      readSessionRuntime({ instance: 'inst-1', session: null, allowed: null }),
    ).toBeUndefined();
    expect(
      readSessionRuntime({ instance: 'inst-1', session: null, allowed: null, runtime: rt }),
    ).toEqual(rt);

    const rtMsg = readServerMessage({ type: 'runtime', runtime: rt });
    expect(rtMsg).toEqual({ type: 'runtime', runtime: rt });

    const helloMsg = readServerMessage({
      type: 'hello',
      vehicle_sil_gui: 4,
      instance: 'inst-1',
      session: null,
      allowed: null,
      replay: null,
      runtime: rt,
    });
    expect(helloMsg?.type === 'hello' && helloMsg.runtime).toEqual(rt);
  });

  it('reads variables summaries and operation reports strictly or leniently with Unreadable', () => {
    const rawVars = {
      state: 123,
      reason: 45,
      peers: [
        {
          name: 'Engine',
          connected: 'yes',
          variables: [
            {
              name: 'mode',
              type: 9,
              role: 7,
              writable: 'true',
              builtin: 'true',
              unit: null,
              min: '0',
              max: Infinity,
              step: 0.5,
              description: false,
              enum: [{ name: 'bad' }, { name: 'ok', value: 1 }],
              availability: 'inactive',
            },
            { name: 42 },
          ],
        },
        { connected: true },
      ],
      peers_omitted: -2,
      variables_omitted: 3,
      operations_from_seq: null,
      operations_open_omitted: 'none',
    };
    expect(() => readVariablesSummary(rawVars)).toThrow(ProtocolError);
    const uVars = new Unreadable('variables');
    const lenientVars = readVariablesReport(rawVars, 'variables', uVars);
    expect(lenientVars).toEqual({
      state: 'unknown',
      reason: null,
      peers: [
        {
          name: 'Engine',
          connected: false,
          variables: [
            {
              name: 'mode',
              type: '',
              role: 'output',
              writable: false,
              builtin: false,
              unit: '',
              min: null,
              max: null,
              step: 0.5,
              description: '',
              enum: [{ name: 'ok', value: 1 }],
              availability: 'inactive',
            },
          ],
        },
      ],
      peers_omitted: 0,
      variables_omitted: 3,
      operations: [],
      operations_from_seq: null,
      operations_open_omitted: 0,
    });
    expect(uVars.parts).toContain('peers.variables.builtin');

    const withBuiltin = (builtin: unknown) => ({
      state: 'ready',
      reason: null,
      peers: [
        {
          name: 'Engine',
          connected: true,
          variables: [
            {
              name: 'ecu.connected',
              type: 'bool',
              role: 'input',
              writable: true,
              ...(builtin === undefined ? {} : { builtin }),
              unit: '',
              min: null,
              max: null,
              step: null,
              description: '',
              enum: null,
              availability: 'active',
            },
          ],
        },
      ],
      peers_omitted: 0,
      variables_omitted: 0,
      operations: [],
      operations_from_seq: 0,
      operations_open_omitted: 0,
    });
    expect(readVariablesSummary(withBuiltin(true)).peers[0].variables[0].builtin).toBe(true);
    expect(readVariablesSummary(withBuiltin(false)).peers[0].variables[0].builtin).toBe(false);
    expect(readVariablesSummary(withBuiltin(undefined)).peers[0].variables[0].builtin).toBe(false);
    for (const invalid of [null, 1, 'true']) {
      expect(() => readVariablesSummary(withBuiltin(invalid))).toThrow(ProtocolError);
    }

    const rawOp = {
      seq: 7,
      state: 'rejected',
      done: 'yes',
      peers: {
        Engine: {
          state: 5,
          applied_at_ms: '10',
          late: 'no',
          reason: 99,
          applied_writes: -1,
          failed_write: 0,
        },
        Broken: 'not-an-object',
      },
    };
    expect(() => readOperationReport(rawOp, 'operation')).toThrow(ProtocolError);
    const uOp = new Unreadable('operation');
    expect(readOperationReport(rawOp, 'operation', uOp)).toEqual({
      seq: 7,
      state: 'rejected',
      done: false,
      peers: {
        Engine: {
          state: 'unknown',
          applied_at_ms: null,
          late: false,
          reason: null,
          applied_writes: null,
          failed_write: 0,
        },
      },
    });
  });
});

type JsonSchema = Record<string, unknown>;

function resolveSchemaRef(root: JsonSchema, ref: string): JsonSchema {
  if (!ref.startsWith('#/')) throw new Error(`unsupported ref: ${ref}`);
  let current: unknown = root;
  for (const part of ref.slice(2).split('/')) {
    if (typeof current !== 'object' || current === null) {
      throw new Error(`dangling ref: ${ref}`);
    }
    current = (current as Record<string, unknown>)[part];
  }
  if (typeof current !== 'object' || current === null) {
    throw new Error(`dangling ref: ${ref}`);
  }
  return current as JsonSchema;
}

function validateAgainstSchema(
  root: JsonSchema,
  rawSchema: JsonSchema,
  value: unknown,
  path = '$',
): string[] {
  const schema =
    typeof rawSchema['$ref'] === 'string' ? resolveSchemaRef(root, rawSchema['$ref']) : rawSchema;

  if ('oneOf' in schema && Array.isArray(schema['oneOf'])) {
    const disc = schema['discriminator'] as
      { propertyName?: string; mapping?: Record<string, string> } | undefined;
    if (
      disc?.propertyName &&
      disc.mapping &&
      typeof value === 'object' &&
      value !== null &&
      !Array.isArray(value)
    ) {
      const key = (value as Record<string, unknown>)[disc.propertyName];
      if (typeof key === 'string' && key in disc.mapping) {
        return validateAgainstSchema(root, { $ref: disc.mapping[key] }, value, path);
      }
      return [`${path}.${disc.propertyName}: unexpected discriminator ${JSON.stringify(key)}`];
    }
    const matches = (schema['oneOf'] as JsonSchema[]).filter(
      (branch) => validateAgainstSchema(root, branch, value, path).length === 0,
    );
    return matches.length === 1
      ? []
      : [`${path}: expected exactly 1 oneOf branch match (got ${matches.length})`];
  }

  if ('anyOf' in schema && Array.isArray(schema['anyOf'])) {
    const ok = (schema['anyOf'] as JsonSchema[]).some(
      (branch) => validateAgainstSchema(root, branch, value, path).length === 0,
    );
    return ok ? [] : [`${path}: value matched none of anyOf branches`];
  }

  if ('const' in schema && value !== schema['const']) {
    return [`${path}: expected ${JSON.stringify(schema['const'])}, got ${JSON.stringify(value)}`];
  }

  if ('enum' in schema && Array.isArray(schema['enum']) && !schema['enum'].includes(value)) {
    return [`${path}: expected one of ${JSON.stringify(schema['enum'])}`];
  }

  if (typeof schema['not'] === 'object' && schema['not'] !== null) {
    if (validateAgainstSchema(root, schema['not'] as JsonSchema, value, path).length === 0) {
      return [`${path}: matched forbidden 'not' subschema`];
    }
  }

  const kind = schema['type'];
  if (typeof kind === 'string') {
    if (kind === 'null' && value !== null) return [`${path}: expected null`];
    if (kind === 'boolean' && typeof value !== 'boolean') return [`${path}: expected boolean`];
    if (kind === 'string' && typeof value !== 'string') return [`${path}: expected string`];
    if (kind === 'integer' && (typeof value !== 'number' || !Number.isInteger(value))) {
      return [`${path}: expected integer`];
    }
    if (kind === 'number' && (typeof value !== 'number' || !Number.isFinite(value))) {
      return [`${path}: expected finite number`];
    }
    if (kind === 'array' && !Array.isArray(value)) return [`${path}: expected array`];
    if (
      kind === 'object' &&
      (typeof value !== 'object' || value === null || Array.isArray(value))
    ) {
      return [`${path}: expected object`];
    }
  }

  if (typeof value === 'string') {
    if (typeof schema['minLength'] === 'number' && value.length < schema['minLength']) {
      return [`${path}: length < ${schema['minLength']}`];
    }
    if (typeof schema['pattern'] === 'string' && !new RegExp(schema['pattern']).test(value)) {
      return [`${path}: does not match ${schema['pattern']}`];
    }
  }

  if (typeof value === 'number') {
    if (typeof schema['minimum'] === 'number' && value < schema['minimum']) {
      return [`${path}: ${value} < ${schema['minimum']}`];
    }
    if (typeof schema['maximum'] === 'number' && value > schema['maximum']) {
      return [`${path}: ${value} > ${schema['maximum']}`];
    }
  }

  if (Array.isArray(value) && typeof schema['items'] === 'object' && schema['items'] !== null) {
    const errors: string[] = [];
    for (const [i, item] of value.entries()) {
      errors.push(
        ...validateAgainstSchema(root, schema['items'] as JsonSchema, item, `${path}[${i}]`),
      );
    }
    return errors;
  }

  if (typeof value === 'object' && value !== null && !Array.isArray(value)) {
    const obj = value as Record<string, unknown>;
    const errors: string[] = [];
    const props = (schema['properties'] as Record<string, JsonSchema> | undefined) ?? {};
    if (
      typeof schema['minProperties'] === 'number' &&
      Object.keys(obj).length < schema['minProperties']
    ) {
      errors.push(`${path}: property count < ${schema['minProperties']}`);
    }
    if (typeof schema['propertyNames'] === 'object' && schema['propertyNames'] !== null) {
      for (const key of Object.keys(obj)) {
        errors.push(
          ...validateAgainstSchema(
            root,
            schema['propertyNames'] as JsonSchema,
            key,
            `${path}.${key}`,
          ),
        );
      }
    }
    if (Array.isArray(schema['required'])) {
      for (const reqKey of schema['required'] as string[]) {
        if (!(reqKey in obj)) errors.push(`${path}.${reqKey}: missing required property`);
      }
    }
    if (schema['additionalProperties'] === false) {
      for (const key of Object.keys(obj)) {
        if (!(key in props)) errors.push(`${path}.${key}: unexpected additional property`);
      }
    }
    if (typeof schema['dependentRequired'] === 'object' && schema['dependentRequired'] !== null) {
      for (const [trigger, needed] of Object.entries(
        schema['dependentRequired'] as Record<string, string[]>,
      )) {
        if (trigger in obj) {
          for (const dep of needed) {
            if (!(dep in obj)) errors.push(`${path}.${dep}: required when ${trigger} is present`);
          }
        }
      }
    }
    for (const [key, propVal] of Object.entries(obj)) {
      if (key in props) {
        errors.push(...validateAgainstSchema(root, props[key], propVal, `${path}.${key}`));
      } else if (
        typeof schema['additionalProperties'] === 'object' &&
        schema['additionalProperties'] !== null
      ) {
        errors.push(
          ...validateAgainstSchema(
            root,
            schema['additionalProperties'] as JsonSchema,
            propVal,
            `${path}.${key}`,
          ),
        );
      }
    }
    return errors;
  }

  return [];
}

describe('generated OpenAPI 3.1 and WebSocket JSON Schemas align with client builders and readers', () => {
  it('covers all 17 HTTP endpoints and validates client request payloads against OpenAPI 3.1', async () => {
    const { openapi } = await protocolSchemas();
    expect(openapi['openapi']).toBe('3.1.0');
    const paths = openapi['paths'] as Record<string, Record<string, JsonSchema>>;

    const expectedRoutes: [string, string][] = [
      ['get', '/api/app'],
      ['get', '/api/configs'],
      ['get', '/api/config'],
      ['post', '/api/config'],
      ['post', '/api/check'],
      ['post', '/api/describe'],
      ['post', '/api/connections'],
      ['post', '/api/prepare'],
      ['post', '/api/run'],
      ['post', '/api/stop-session'],
      ['post', '/api/time'],
      ['post', '/api/write'],
      ['get', '/api/session'],
      ['get', '/api/dashboard'],
      ['get', '/api/scenarios'],
      ['get', '/api/recording'],
      ['get', '/api/timeline/{run_id}'],
    ];
    for (const [method, route] of expectedRoutes) {
      expect(paths[route]?.[method], `${method.toUpperCase()} ${route}`).toBeDefined();
    }

    const reqSchemaOf = (route: string): JsonSchema => {
      const op = paths[route]['post'];
      const rb = op['requestBody'] as { content: { 'application/json': { schema: JsonSchema } } };
      return rb.content['application/json'].schema;
    };

    const allHttpExamples = new Map((await httpExamples()).map((e) => [e.name, e.response]));
    const recorded: HttpRequest[] = [];
    const http: HttpTransport = (req, done) => {
      recorded.push(req);
      const path = req.path.split('?')[0];
      const replies: Record<string, unknown> = {
        'api/prepare': { instance: 'inst-1' },
        'api/check': allHttpExamples.get('check-passed'),
        'api/describe': allHttpExamples.get('describe'),
        'api/config': { file: 'simulation.yaml', stamp: 'a'.repeat(64) },
        'api/connections': allHttpExamples.get('connections'),
        'api/run': { instance: 'inst-1', run_id: 'inst-1-1' },
        'api/stop-session': { instance: 'inst-1', run_id: 'inst-1-1' },
        'api/time': { run_id: 'inst-1-1', command: { command: 'pause' } },
        'api/write': { run_id: 'inst-1-1', operation: 1 },
      };
      done({ ok: true, body: replies[path] ?? {} });
      return () => undefined;
    };

    vi.useFakeTimers();
    try {
      const sockets = new FakeWebSockets();
      const client = createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        token: 'secret-token',
        http,
        socket: sockets.factory,
      });

      // Bring WS session online so targeted stop / time / write build real targeted requests
      const runningResp = allHttpExamples.get('session-running') as {
        instance: string;
        session: Record<string, unknown>;
        allowed: Record<string, unknown>;
        runtime: Record<string, unknown>;
      };
      client.connect();
      const sock = sockets.last();
      sock.open();
      sock.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.4.0',
        instance: 'inst-1',
        stream_seq: 1,
        session: {
          ...runningResp.session,
          id: 'inst-1-1',
          variables: {
            state: 'ready',
            reason: null,
            peers: [],
            peers_omitted: 0,
            variables_omitted: 0,
            operations: [],
            operations_from_seq: 0,
            operations_open_omitted: 0,
          },
        },
        allowed: runningResp.allowed,
        replay: null,
        runtime: runningResp.runtime,
      });
      sock.send({ type: 'synced', stream_seq: 1, run_id: 'inst-1-1', last_seq: 1 });
      vi.advanceTimersByTime(100);

      await client.prepare({ file: 'simulation.yaml', disable: ['Logger'] });
      await client.check({ file: 'simulation.yaml', disable: ['Dashboard'] });
      await client.check({ file: 'simulation.yaml', text: 'vehicle_sil_simulation: 2\n' });
      await client.describe({ file: 'simulation.yaml' });
      await client.saveConfig({
        file: 'simulation.yaml',
        text: 'vehicle_sil_simulation: 2\n',
        stamp: 'a'.repeat(64),
      });
      await client.connections({ file: 'connect.yaml', network: 'ETH' });
      await client.run.stop();
      expect(client.time.step(100)).toEqual({ sent: true });
      // Clear waiting state via a command event so a second time command is emitted
      sock.send({
        type: 'event',
        stream_seq: 2,
        run_id: 'inst-1-1',
        event: {
          event: 'command',
          seq: 99,
          command: 'step',
          source: 'stdin',
          accepted: true,
          message: null,
          reason: null,
          ms: 100,
          at_ms: 100,
          id: null,
        },
      });
      vi.advanceTimersByTime(100);
      expect(client.time.setSpeed('max')).toEqual({ sent: true });
      const writePromise = client.variables.write({ Engine: { 'engine.demand': 40 } });
      // Settle the write operation cleanly so no dangling rejection happens on client.close()
      sock.send({
        type: 'operation',
        stream_seq: 3,
        run_id: 'inst-1-1',
        seq: 1,
        state: 'applied',
        done: true,
        peers: {
          Engine: {
            state: 'applied',
            applied_at_ms: 100,
            late: false,
            reason: null,
            applied_writes: 1,
            failed_write: null,
          },
        },
      });
      vi.advanceTimersByTime(100);
      await writePromise;

      const watch = client.variables.watch([{ peer: 'Engine', name: 'engine.speed' }], {
        periodMs: 100,
      });
      watch.close();

      // Start run on a fresh idle client
      const idleClient = createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        token: 'secret-token',
        http,
        socket: sockets.factory,
      });
      await idleClient.run.start({
        file: 'simulation.yaml',
        disable: [],
        speed: 'max',
        start_paused: true,
      });

      const postRequests = recorded.filter((r) => r.method === 'POST');
      expect(postRequests.map((r) => r.path)).toEqual(
        expect.arrayContaining([
          'api/prepare',
          'api/check',
          'api/describe',
          'api/config',
          'api/connections',
          'api/stop-session',
          'api/time',
          'api/write',
          'api/run',
        ]),
      );
      for (const req of postRequests) {
        const route = `/${req.path.split('?')[0]}`;
        const errors = validateAgainstSchema(openapi, reqSchemaOf(route), req.body, route);
        expect(errors, `${route}: ${JSON.stringify(req.body)}`).toEqual([]);
      }

      // Verify negative schema checks catch targeting & extra-key drift
      expect(
        validateAgainstSchema(openapi, reqSchemaOf('/api/time'), {
          command: 'pause',
          run_id: 'inst-1-1',
        }).length,
      ).toBeGreaterThan(0);
      expect(
        validateAgainstSchema(openapi, reqSchemaOf('/api/prepare'), {
          file: 'simulation.yaml',
          unknown_field: 1,
        }).length,
      ).toBeGreaterThan(0);
      expect(
        validateAgainstSchema(openapi, reqSchemaOf('/api/prepare'), {
          file: 'simulation.yaml',
          variant: null,
        }).length,
      ).toBeGreaterThan(0);
      expect(
        validateAgainstSchema(openapi, reqSchemaOf('/api/run'), {
          file: 'simulation.yaml',
          connect: { '': 'eth0' },
        }).length,
      ).toBeGreaterThan(0);
      expect(
        validateAgainstSchema(openapi, reqSchemaOf('/api/write'), {
          run_id: 'inst-1-1',
          writes: {},
        }).length,
      ).toBeGreaterThan(0);

      client.close();
      idleClient.close();
    } finally {
      vi.useRealTimers();
    }
  });

  it('validates client WS frames against ws-client.schema.json and covers all 13 ws-server types', async () => {
    const { wsClient, wsServer } = await protocolSchemas();
    const runningResp = (await httpExamples()).find((e) => e.name === 'session-running')
      ?.response as {
      session: Record<string, unknown>;
      allowed: Record<string, unknown>;
      runtime: Record<string, unknown>;
    };
    const sockets = new FakeWebSockets();
    vi.useFakeTimers();
    let sentFrames: unknown[];
    try {
      const client = createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        token: 'secret-token',
        http: (_req, done) => {
          done({ ok: true, body: {} });
          return () => undefined;
        },
        socket: sockets.factory,
      });
      client.connect();
      const sock = sockets.last();
      sock.open();
      sock.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.4.0',
        instance: 'inst-1',
        stream_seq: 1,
        session: { ...runningResp.session, id: 'inst-1-1' },
        allowed: runningResp.allowed,
        replay: null,
        runtime: runningResp.runtime,
      });
      sock.send({ type: 'synced', stream_seq: 1, run_id: 'inst-1-1', last_seq: 1 });
      vi.advanceTimersByTime(100);
      const watch = client.variables.watch('all', { periodMs: 100 });
      await Promise.resolve();
      watch.close();
      await Promise.resolve();
      sentFrames = [...sock.sent];
      client.close();
    } finally {
      vi.useRealTimers();
    }

    expect(sentFrames.map((m) => (m as { type: string }).type)).toEqual([
      'auth',
      'watch',
      'unwatch',
    ]);
    for (const [i, frame] of sentFrames.entries()) {
      expect(
        validateAgainstSchema(wsClient, wsClient, frame, `ws-client[${i}]`),
        JSON.stringify(frame),
      ).toEqual([]);
    }

    // Every server->client frame type in ws-server.schema.json is seen in examples and read by readServerMessage
    const mapping = (wsServer['discriminator'] as { mapping: Record<string, string> }).mapping;
    const schemaTypes = Object.keys(mapping).sort();
    expect(schemaTypes).toEqual([
      'allowed',
      'availability',
      'dropped',
      'error',
      'event',
      'hello',
      'operation',
      'resync',
      'runtime',
      'session',
      'synced',
      'values',
      'variables',
    ]);
    const exampleTypes = new Set(
      (await webSocketExamples()).flatMap((ex) =>
        ex.server.map((m) => (m as { type: string }).type),
      ),
    );
    // `runtime` is emitted live on prepare/run transitions; validate it against ws-server.schema.json and readServerMessage
    const runtimeFrame = {
      type: 'runtime',
      stream_seq: 4,
      runtime: {
        mode: 'hot',
        state: 'ready',
        config: 'simulation.yaml',
        reason: null,
        message: null,
      },
    };
    expect(validateAgainstSchema(wsServer, wsServer, runtimeFrame, 'ws-runtime')).toEqual([]);
    expect(readServerMessage(runtimeFrame)).toMatchObject({ type: 'runtime' });
    exampleTypes.add('runtime');
    expect([...exampleTypes].sort()).toEqual(schemaTypes);
  });

  it('reads Recording.end_ms and SummaryScenario.handed_over cleanly', () => {
    const baseRecording = {
      run_id: '1',
      file: 'sim.yaml',
      state: 'ended',
      available: true,
      reason: null,
      writes: 1,
      omitted: 0,
      steps: 1,
      yaml: 'vehicle_sil_scenario: 1\nend_ms: 1200\n',
    };
    expect(readRecording({ recording: { ...baseRecording, end_ms: 1200 } })?.end_ms).toBe(1200);
    expect(readRecording({ recording: { ...baseRecording, end_ms: null } })?.end_ms).toBeNull();
    expect(readRecording({ recording: baseRecording })?.end_ms).toBeNull();
    expect(() => readRecording({ recording: { ...baseRecording, end_ms: 12.5 } })).toThrow(
      ProtocolError,
    );
    expect(() => readRecording({ recording: { ...baseRecording, end_ms: -1 } })).toThrow(
      ProtocolError,
    );
    expect(() => readRecording({ recording: { ...baseRecording, writes: -1 } })).toThrow(
      ProtocolError,
    );

    const runningSummary = {
      id: '1',
      state: 'running',
      launcher: { running: true, exit_code: null, signal: null, silent_s: null },
      stop: 'none',
      file: 'sim.yaml',
      disable: [],
      manual: [],
      speed: 1,
      start_paused: false,
      phase: 'running',
      last_seq: 1,
      last_t_ms: 10,
      time: null,
      last_command: null,
      launcher_state: null,
      allowed: null,
      check: null,
      participants: [],
      findings: [],
      result: null,
      log: {},
      problems: [],
      problems_omitted: 0,
      scenario: {
        file: 'scenarios/a.yaml',
        name: 'a',
        step_count: 1,
        steps: [],
        steps_omitted: 0,
        status: 'passed',
        over_at_ms: 500,
        timeline_url: null,
      },
    };
    const withoutHandedOver = readSessionResponse({ session: runningSummary });
    expect(withoutHandedOver?.scenario?.handed_over).toBeUndefined();
    expect(withoutHandedOver?.unreadable).toEqual([]);

    const withHandedOver = readSessionResponse({
      session: { ...runningSummary, scenario: { ...runningSummary.scenario, handed_over: true } },
    });
    expect(withHandedOver?.scenario?.handed_over).toBe(true);
    expect(withHandedOver?.unreadable).toEqual([]);

    const badHandedOver = readSessionResponse({
      session: {
        ...runningSummary,
        scenario: { ...runningSummary.scenario, handed_over: 'yes' },
      },
    });
    expect(badHandedOver?.scenario?.handed_over).toBe(false);
    expect(badHandedOver?.unreadable).toContain('scenario.handed_over');
  });

  it('reads check scenario case metadata, param_meta, missing_params, and top-level variant/params', () => {
    const check = readCheckResponse({
      check: {
        vehicle_sil_check: 1,
        file: 'sub/simulation.yaml',
        result: 'usage',
        errors: 1,
        warnings: 0,
        notes: 0,
        variant: 'hil',
        variants: ['hil', 'sil'],
        params: [{ name: 'throttle', value: 50, from: 'default' }],
        participants: [
          {
            name: 'Engine',
            line: 4,
            required: true,
            enabled: true,
            manual: false,
            step_ms: 10,
            status: 'described',
            variables: null,
            control: null,
          },
        ],
        networks: [],
        findings: [
          {
            severity: 'error',
            code: 'scenario-case-required',
            file: 'sub/scenarios/cases.yaml',
            line: 1,
            participant: null,
            network: null,
            message: 'choose one case with --case',
            detail: null,
          },
        ],
        hot_eligibility: { eligible: true, reason: null },
        scenario: {
          file: 'sub/scenarios/cases.yaml',
          name: 'drive cases',
          case: null,
          cases: ['idle', 'full_load'],
          case_details: [
            { name: 'idle', only: null },
            { name: 'full_load', only: ['hil'] },
          ],
          steps: 0,
          params: [
            {
              name: 'mode',
              value: 'eco',
              from: 'default',
              type: 'enum',
              values: ['eco', 'sport'],
              description: 'drive mode',
            },
          ],
          param_meta: [
            {
              name: 'mode',
              type: 'enum',
              values: ['eco', 'sport'],
              description: 'drive mode',
            },
            {
              name: 'target_rpm',
              type: 'integer',
              description: 'target engine rpm',
            },
          ],
          missing_params: ['target_rpm'],
        },
      },
    });

    expect(check.result).toBe('usage');
    expect(check.variant).toBe('hil');
    expect(check.variants).toEqual(['hil', 'sil']);
    expect(check.params).toEqual([{ name: 'throttle', value: 50, from: 'default' }]);
    expect(check.raw?.['hot_eligibility']).toEqual({ eligible: true, reason: null });
    expect(check.scenario).toMatchObject({
      file: 'sub/scenarios/cases.yaml',
      name: 'drive cases',
      case: null,
      cases: ['idle', 'full_load'],
      case_details: [
        { name: 'idle', only: null },
        { name: 'full_load', only: ['hil'] },
      ],
      params: [
        {
          name: 'mode',
          value: 'eco',
          from: 'default',
          type: 'enum',
          values: ['eco', 'sport'],
          description: 'drive mode',
        },
      ],
      param_meta: [
        {
          name: 'mode',
          type: 'enum',
          values: ['eco', 'sport'],
          description: 'drive mode',
        },
        {
          name: 'target_rpm',
          type: 'integer',
          description: 'target engine rpm',
        },
      ],
      missing_params: ['target_rpm'],
    });
  });

  it('preserves scenario.case, scenario.params, step provenance, and session.intent in readSessionResponse', () => {
    const callStack = [
      {
        file: 'sub/scenarios/cases.yaml',
        line: 31,
        name: 'run load check',
        params: { throttle: 50, label: 'rated' },
      },
    ];
    const summary = readSessionResponse({
      session: {
        id: '7',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        stop: 'none',
        file: 'sub/simulation.yaml',
        disable: [],
        manual: [],
        speed: 1,
        start_paused: false,
        phase: 'done',
        last_seq: 12,
        last_t_ms: 400,
        time: null,
        last_command: null,
        launcher_state: null,
        allowed: null,
        check: null,
        participants: [],
        findings: [],
        result: null,
        log: {},
        problems: [],
        problems_omitted: 0,
        intent: {
          scenario: 'sub/scenarios/cases.yaml',
          case: 'full_load',
          after_scenario: 'pause',
          variant: 'hil',
          disable: ['Logger'],
          enable: ['Engine'],
          manual: ['Brake'],
          connect: { Engine: 'lab' },
          params: { target_rpm: 3000, mode: 'sport', dry_run: false },
          time_policy: 'realtime',
          speed: 2,
          start_paused: true,
          stop_at_ms: 5000,
        },
        scenario: {
          file: 'sub/scenarios/cases.yaml',
          name: 'drive cases',
          case: 'full_load',
          params: [
            {
              name: 'target_rpm',
              value: 3000,
              from: 'cli',
              type: 'integer',
              description: 'target engine rpm',
            },
          ],
          step_count: 1,
          steps: [
            {
              index: 1,
              file: 'sub/scenarios/templates/check.yaml',
              line: 22,
              kind: 'expect',
              name: 'rated: speed check',
              text: 'expect Engine.speed == 3000',
              status: 'failed',
              start_ms: 100,
              at_ms: 200,
              end_ms: 200,
              code: 'expect-failed',
              message: 'actual 2500',
              call_stack: callStack,
            },
          ],
          steps_omitted: 0,
          status: 'failed',
          over_at_ms: 200,
          timeline_url: 'api/timeline/7',
        },
      },
    });

    expect(summary?.unreadable).toEqual([]);
    expect(summary?.intent).toEqual({
      scenario: 'sub/scenarios/cases.yaml',
      case: 'full_load',
      after_scenario: 'pause',
      variant: 'hil',
      disable: ['Logger'],
      enable: ['Engine'],
      manual: ['Brake'],
      connect: { Engine: 'lab' },
      params: { target_rpm: 3000, mode: 'sport', dry_run: false },
      time_policy: 'realtime',
      speed: 2,
      start_paused: true,
      stop_at_ms: 5000,
    });
    expect(summary?.scenario?.case).toBe('full_load');
    expect(summary?.scenario?.params).toEqual([
      {
        name: 'target_rpm',
        value: 3000,
        from: 'cli',
        type: 'integer',
        description: 'target engine rpm',
      },
    ]);
    expect(summary?.scenario?.steps[0]).toMatchObject({
      file: 'sub/scenarios/templates/check.yaml',
      line: 22,
      name: 'rated: speed check',
      call_stack: callStack,
    });

    const malformed = readSessionResponse({
      session: {
        id: '8',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        stop: 'none',
        file: 'sub/simulation.yaml',
        disable: [],
        manual: [],
        speed: null,
        start_paused: false,
        phase: null,
        last_seq: null,
        last_t_ms: null,
        time: null,
        last_command: null,
        launcher_state: null,
        allowed: null,
        check: null,
        participants: [],
        findings: [],
        result: null,
        log: {},
        problems: [],
        problems_omitted: 0,
        intent: { scenario: 123, case: '', params: { bad: { nested: true } } },
        scenario: null,
      },
    });
    expect(malformed?.unreadable).toEqual(
      expect.arrayContaining(['intent.scenario', 'intent.case', 'intent.params.bad']),
    );
  });
});
