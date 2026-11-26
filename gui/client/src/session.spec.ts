import { GuiClient, createClient } from './client';
import { BATCH_MS, backoffMs } from './connection';
import { STALE_EVENTS_NOTICE } from './session';
import { runOutcome } from './session-state';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { WebSocketExample, httpExamples, webSocketExamples } from './testing/protocol-examples';

/** What the tests read (the readable values of the session copy, as functions that return the value). */
function view(client: GuiClient) {
  const feed = client.session;
  return {
    session: () => feed.session.get(),
    ready: () => feed.ready.get(),
    notices: () => feed.notices.get(),
    rejected: () => feed.rejected.get(),
    tryAgainReason: () => client.connection.tryAgainReason.get(),
    incompatible: () => feed.incompatible.get(),
  };
}

describe('SessionFeed', () => {
  let fake: FakeWebSockets;
  let examples: Map<string, WebSocketExample>;
  let clients: GuiClient[];

  beforeAll(async () => {
    examples = new Map((await webSocketExamples()).map((example) => [example.name, example]));
  });

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    clients = [];
  });

  afterEach(() => {
    for (const client of clients) client.close();
    vi.useRealTimers();
  });

  /** Feeds the server notifications of an example as is, in the order they arrived. */
  function play(socket: FakeSocket, name: string, from = 0, to?: number) {
    const example = examples.get(name);
    if (!example) throw new Error(`no example ${name}`);
    for (const message of example.server.slice(from, to)) socket.send(message);
    vi.advanceTimersByTime(BATCH_MS);
  }

  const clientsLast = () => clients[clients.length - 1];

  /** Creates a client, connects and opens (no HTTP). */
  function open(http = new FakeHttp()): { run: ReturnType<typeof view>; socket: FakeSocket } {
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      socket: fake.factory,
      http: http.transport,
    });
    clients.push(client);
    client.connect();
    const socket = fake.last();
    socket.open();
    return { run: view(client), socket };
  }

  it('exposes the authenticated instance even when no run exists', () => {
    const { socket } = open();
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'new-server',
      stream_seq: 0,
      session: null,
      replay: null,
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(clientsLast().session.currentInstance()).toBe('new-server');
    socket.fail();
    expect(clientsLast().session.currentInstance()).toBe('new-server');
  });

  it('authenticates first, then follows a whole run', () => {
    const { run, socket } = open();
    expect(socket.greeting).toEqual([{ type: 'auth', token: 'secret' }]);
    expect(run.session()).toBeNull();
    expect(run.ready()).toBe(false);

    play(socket, 'ws-pass');
    expect(run.ready()).toBe(true);
    const session = run.session();
    expect(session?.id).toBe('1');
    expect(session?.file).toBe('simulation.yaml');
    expect(session?.result?.result).toBe('passed');
    expect(session?.phase).toBe('ended');
    expect(session && runOutcome(session)).toBe('ended');
    expect(session?.participants.map((p) => [p.name, p.state])).toContainEqual(['Engine', 'ended']);
    // In the example ECUs, Dashboard writes the lines
    const dashboardLog = session?.logs.find((line) => line.source === 'Dashboard');
    expect(dashboardLog).toBeDefined();
    expect(typeof dashboardLog?.tMs).toBe('number');
    expect(session?.check?.result).toBe('passed');
  });

  it('takes the allowed message and an unknown message type without changing the session', () => {
    const { run, socket } = open();
    play(socket, 'ws-pass');
    const before = run.session();
    const seen: string[] = [];
    clientsLast().session.onMessages((batch) => seen.push(...batch.map((m) => m.type)));
    const allowed = { start: { ok: true, reason: null, message: null } };
    socket.send({
      type: 'allowed',
      stream_seq: 999,
      run_id: null,
      allowed: {
        ...allowed,
        stop: allowed.start,
        force: allowed.start,
        save: { ...allowed.start, caution: null },
      },
    });
    socket.send({ type: 'a-message-of-a-later-version', run_id: '1' });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()).toBe(before); // the same session copy: nothing changed
    expect(seen).toEqual(['allowed']); // the one it knows is passed on, the unknown one is skipped
  });

  it('keeps the check of the hello when later summaries come without it', () => {
    const { run, socket } = open();
    const messages = (examples.get('ws-pass')?.server ?? []).map((m) => {
      const message = structuredClone(m) as { type: string; session?: Record<string, unknown> };
      if (message.type === 'session' && message.session) delete message.session['check'];
      return message;
    });
    for (const message of messages) socket.send(message);
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()?.check?.result).toBe('passed');
    expect(run.session()?.unreadable).toEqual([]);
  });

  it('stops on a refused token even when a subscriber throws, and tells onError', () => {
    const errors: unknown[] = [];
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      socket: fake.factory,
      http: () => undefined,
      onError: (error) => errors.push(error),
    });
    clients.push(client);
    client.session.onMessages(() => {
      throw new Error('a broken subscriber');
    });
    client.session.session.subscribe(() => {
      throw new Error('another broken subscriber');
    });
    client.connect();
    const socket = fake.last();
    socket.open();
    play(socket, 'ws-unauthorized');
    expect(client.session.rejected.get()).toBe(true);
    expect(socket.closedByClient).toBe(true);
    socket.close(4401, 'unauthorized');
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
    expect(errors.map(String)).toContain('Error: a broken subscriber');
    // connect() after a refusal does nothing
    client.connect();
    expect(fake.sockets).toHaveLength(1);
  });

  it('does not connect again from a subscriber that sees the connection close after a refusal', () => {
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      socket: fake.factory,
      http: new FakeHttp().transport,
    });
    clients.push(client);
    client.connection.state.subscribe((state) => {
      if (state === 'closed') client.connect();
    });
    client.connect();
    fake.last().open();
    play(fake.last(), 'ws-unauthorized');
    expect(client.session.rejected.get()).toBe(true);
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
  });

  it('stops and says why when the server speaks another version', async () => {
    const http = new FakeHttp();
    const { run, socket } = open(http);
    const hello = examples.get('ws-replay')?.server[0] as Record<string, unknown>;
    socket.send({ ...hello, vehicle_sil_gui: 2 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(socket.closedByClient).toBe(true);
    expect(run.ready()).toBe(false);
    expect(run.incompatible()).toContain('server speaks version 2');
    await expect(clientsLast().run.start({ file: 'sim.yaml', disable: [] })).rejects.toThrow(
      'use a client made for the server',
    );
    http.expectNone('api/run');
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
  });

  it('says so when a message is not JSON, and builds the state again from hello', () => {
    const { run, socket } = open();
    play(socket, 'ws-pass', 0, 10);
    expect(run.ready()).toBe(true);
    socket.sendText('<html>');
    expect(run.notices().map((n) => n.message)).toEqual([
      'unreadable message: not JSON (connecting again): <html>',
    ]);
    expect(run.ready()).toBe(false);
    vi.advanceTimersByTime(backoffMs(1));
    const again = fake.last();
    expect(again).not.toBe(socket);
    again.open();
    play(again, 'ws-replay');
    expect(run.ready()).toBe(true);
  });

  it('builds the same state from the replay of a screen that connects later (multi-tab convergence)', () => {
    const live = open();
    play(live.socket, 'ws-pass');
    const expected = live.run.session();

    const later = open();
    play(later.socket, 'ws-replay');
    const replayed = later.run.session();
    expect(replayed?.result).toEqual(expected?.result);
    expect(replayed?.participants).toEqual(expected?.participants);
    expect(replayed?.check).toEqual(expected?.check);
    expect(replayed?.phase).toBe('ended');
  });

  it('sends instance, run_id, and after_stream_seq on reconnect and appends delta logs without duplicates', async () => {
    const { run, socket } = open();
    const runningExample = (await httpExamples()).find((item) => item.name === 'session-running');
    const summary = (runningExample?.response as { session: Record<string, unknown> }).session;
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: '0123456789abcdef',
      stream_seq: 10,
      session: { ...summary, id: '1', last_seq: 2 },
      replay: { after_stream_seq: null },
    });
    socket.send({
      type: 'event',
      stream_seq: 8,
      run_id: '1',
      event: { event: 'log', seq: 1, t_ms: 10, source: 'Engine', line: 'line 1' },
    });
    socket.send({
      type: 'event',
      stream_seq: 9,
      run_id: '1',
      event: { event: 'log', seq: 2, t_ms: 20, source: 'Engine', line: 'line 2' },
    });
    socket.send({
      type: 'synced',
      stream_seq: 10,
      run_id: '1',
      last_seq: 2,
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.ready()).toBe(true);
    expect(run.session()?.logs.map((l) => [l.line, l.tMs])).toEqual([
      ['line 1', 10],
      ['line 2', 20],
    ]);

    socket.fail();
    vi.advanceTimersByTime(backoffMs(1));
    expect(run.ready()).toBe(false);
    const again = fake.last();
    expect(again).not.toBe(socket);
    again.open();
    expect(again.greeting).toEqual([
      {
        type: 'auth',
        token: 'secret',
        instance: '0123456789abcdef',
        run_id: '1',
        after_stream_seq: 10,
      },
    ]);

    // Server responds with delta replay from after_stream_seq: 10
    again.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: '0123456789abcdef',
      stream_seq: 12,
      session: { ...summary, id: '1', last_seq: 3 },
      replay: { after_stream_seq: 10 },
    });
    again.send({
      type: 'event',
      stream_seq: 11,
      run_id: '1',
      event: { event: 'log', seq: 3, t_ms: 30, source: 'Engine', line: 'line 3' },
    });
    again.send({
      type: 'synced',
      stream_seq: 12,
      run_id: '1',
      last_seq: 3,
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.ready()).toBe(true);
    expect(run.session()?.logs.map((l) => [l.line, l.tMs])).toEqual([
      ['line 1', 10],
      ['line 2', 20],
      ['line 3', 30],
    ]);
  });

  it('keeps after_stream_seq unchanged when delayed values frames arrive outside stream_seq', async () => {
    const { socket } = open();
    const runningExample = (await httpExamples()).find((item) => item.name === 'session-running');
    const summary = (runningExample?.response as { session: Record<string, unknown> }).session;
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'inst-1',
      stream_seq: 5,
      session: { ...summary, id: 'run-1', last_seq: 1 },
      replay: { after_stream_seq: null },
    });
    socket.send({ type: 'synced', stream_seq: 5, run_id: 'run-1', last_seq: 1 });
    socket.send({
      type: 'values',
      id: 1,
      watch_id: 1,
      run_id: 'run-1',
      sim_ms: 100,
      paused: false,
      peers: { Engine: { sample_ms: 100, values: { 'engine.speed': 900 } } },
    });
    vi.advanceTimersByTime(BATCH_MS);

    socket.fail();
    vi.advanceTimersByTime(backoffMs(1));
    const again = fake.last();
    again.open();
    expect(again.greeting).toEqual([
      {
        type: 'auth',
        token: 'secret',
        instance: 'inst-1',
        run_id: 'run-1',
        after_stream_seq: 5,
      },
    ]);
  });

  it('rebuilds from scratch when the server instance changes or on full replay after resync', async () => {
    const { run, socket } = open();
    const runningExample = (await httpExamples()).find((item) => item.name === 'session-running');
    const summary = (runningExample?.response as { session: Record<string, unknown> }).session;
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'server-a',
      stream_seq: 3,
      session: { ...summary, id: '1', last_seq: 1 },
      replay: { after_stream_seq: null },
    });
    socket.send({
      type: 'event',
      stream_seq: 2,
      run_id: '1',
      event: { event: 'log', seq: 1, t_ms: 10, source: 'Engine', line: 'old server log' },
    });
    socket.send({ type: 'synced', stream_seq: 3, run_id: '1', last_seq: 1 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()?.logs.map((l) => l.line)).toEqual(['old server log']);

    // Server restarts with a new instance even if run_id happens to be '1'
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'server-b',
      stream_seq: 2,
      session: { ...summary, id: '1', last_seq: 1 },
      replay: { after_stream_seq: null },
    });
    socket.send({
      type: 'event',
      stream_seq: 1,
      run_id: '1',
      event: { event: 'log', seq: 1, t_ms: 10, source: 'Engine', line: 'new server log' },
    });
    socket.send({ type: 'synced', stream_seq: 2, run_id: '1', last_seq: 1 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(clientsLast().session.currentInstance()).toBe('server-b');
    expect(clientsLast().session.currentTarget()).toEqual({ instance: 'server-b', run_id: '1' });
    expect(run.session()?.logs.map((l) => l.line)).toEqual(['new server log']);
  });

  it('switches runs cleanly and computes elapsedMs from run_t0_ms', async () => {
    const { run, socket } = open();
    const runningExample = (await httpExamples()).find((item) => item.name === 'session-running');
    const summary = (runningExample?.response as { session: Record<string, unknown> }).session;
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'inst-1',
      stream_seq: 2,
      session: { ...summary, id: 'inst-1-1', run_t0_ms: 100, last_t_ms: 250, last_seq: 1 },
      replay: { after_stream_seq: null },
    });
    socket.send({ type: 'synced', stream_seq: 2, run_id: 'inst-1-1', last_seq: 1 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()?.id).toBe('inst-1-1');
    expect(run.session()?.elapsedMs).toBe(150);

    // Next hot run starts at t_ms = 5000
    socket.send({
      type: 'session',
      stream_seq: 3,
      session: { ...summary, id: 'inst-1-2', run_t0_ms: 5000, last_t_ms: 5000, last_seq: 10 },
    });
    socket.send({
      type: 'event',
      stream_seq: 4,
      run_id: 'inst-1-2',
      event: { event: 'time', seq: 11, t_ms: 5320, sim_ms: 300 },
    });
    // Stale event from previous run is ignored
    socket.send({
      type: 'event',
      stream_seq: 5,
      run_id: 'inst-1-1',
      event: { event: 'log', seq: 2, t_ms: 9999, source: 'Engine', line: 'stale run 1' },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()?.id).toBe('inst-1-2');
    expect(run.session()?.elapsedMs).toBe(320);
    expect(run.session()?.logs).toEqual([]);
  });

  it('ends a run whose launcher died without a result, and keeps what the server noticed', () => {
    const { run, socket } = open();
    play(socket, 'ws-launcher-crash');
    const session = run.session();
    expect(session && runOutcome(session)).toBe('crashed');
    expect(session?.launcher.running).toBe(false);
    expect(session?.result).toBeNull();
    expect(session?.problems.map((p) => p.code)).toContain('no-result');
  });

  it('keeps the server notes of a launcher that writes bad lines', () => {
    const { run, socket } = open();
    play(socket, 'ws-bad-lines');
    const codes = run.session()?.problems.map((p) => p.code);
    expect(codes).toContain('seq-gap');
    expect(codes).toContain('launcher-output');
    expect(run.session()?.result?.result).toBe('passed');
  });

  it('stops reconnecting when the server refuses the token', () => {
    const { run, socket } = open();
    play(socket, 'ws-unauthorized');
    expect(run.rejected()).toBe(true);
    socket.close();
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
    expect(run.session()).toBeNull();
  });

  it('waits longer each time the server says to try again later, and does not take it as a refused token', () => {
    const { run, socket } = open();
    socket.close(1013, 'too-many-connections');
    expect(run.tryAgainReason()).toBe('too-many-connections');
    expect(run.rejected()).toBe(false);
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);

    fake.last().open();
    fake.last().close(1013, 'too-many-unauthenticated');
    expect(run.tryAgainReason()).toBe('too-many-unauthenticated');
    vi.advanceTimersByTime(backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);

    // Once reconnected and a notification arrives, the reason is cleared and the next delay starts over
    fake.last().open();
    play(fake.last(), 'ws-pass', 0, 1);
    expect(run.tryAgainReason()).toBeNull();
    fake.last().close(1013, 'too-many-connections');
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(4);
  });

  it('counts a notice that comes again instead of repeating it', () => {
    const { run, socket } = open();
    for (let i = 0; i < 3; i++) socket.send({ type: 'event', run_id: '1' });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.notices()).toHaveLength(1);
    expect(run.notices()[0].count).toBe(3);
  });

  it('keeps a valid prefix when a later malformed message restarts the same connection', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'session-running');
    const response = example?.response as { session: Record<string, unknown> };
    const received: string[] = [];
    client.session.onMessages((batch) => received.push(...batch.map((message) => message.type)));
    socket.send({
      type: 'session',
      stream_seq: 10,
      session: { ...response.session, file: 'prefix.yaml' },
    });
    socket.send({ type: 'session', stream_seq: 11, session: { id: '' } });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.file).toBe('prefix.yaml');
    expect(received).toContain('session');
  });

  it('does not take an unreadable hello as in step, and connects again', () => {
    const { run, socket } = open();
    const hello = examples.get('ws-replay')?.server[0] as Record<string, unknown>;
    socket.send({ ...hello, session: { id: '' } });
    socket.send({ type: 'synced', run_id: null, last_seq: null });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.ready()).toBe(false);
    expect(run.notices()[0].message).toContain('unreadable message: message.session.id');
    // Reconnects after a delay and gets in sync from a readable hello
    vi.advanceTimersByTime(backoffMs(1));
    const again = fake.last();
    expect(again).not.toBe(socket);
    again.open();
    play(again, 'ws-replay');
    expect(run.ready()).toBe(true);
    expect(run.session()?.result?.result).toBe('passed');
  });

  it('reads a summary whose result the server cut, and names what it cannot show', () => {
    const { run, socket } = open();
    play(socket, 'ws-kept-limit');
    expect(run.ready()).toBe(true);
    const session = run.session();
    expect(session && runOutcome(session)).toBe('ended');
    expect(session?.result?.result).toBe('passed');
    expect(session?.unreadable).toEqual(['result.participants', 'result.findings']);
    expect(session?.problems.map((p) => p.code)).toContain('kept-limit');
    // The same summary arriving as session on a screen that was open updates cleanly
    const hello = examples.get('ws-kept-limit')?.server[0] as { session: object };
    const live = open();
    play(live.socket, 'ws-pass', 0, 2);
    live.socket.send({ type: 'session', stream_seq: 100, session: hello.session });
    vi.advanceTimersByTime(BATCH_MS);
    expect(live.run.session()?.launcher.running).toBe(false);
    expect(live.run.session()?.result?.result).toBe('passed');
  });

  it('keeps what it can read of events and summaries whose loose fields are missing or of other types', () => {
    const { run, socket } = open();
    play(socket, 'ws-loose-fields');
    expect(run.notices()).toEqual([]);
    const session = run.session();
    expect(session?.result?.result).toBe('passed');
    expect(session?.findings.map((f) => f.message)).toContain(
      'a finding with only a severity and a message',
    );
    expect(session?.unreadable).toEqual(
      expect.arrayContaining(['participants.message', 'findings.code']),
    );
    const logWithoutTms = session?.logs.find((line) => line.line === 'a log event without t_ms');
    expect(logWithoutTms).toBeDefined();
    expect(logWithoutTms?.tMs).toBeNull();
  });

  it('shows the fields the server left out of a large participant event', () => {
    const { run, socket } = open();
    play(socket, 'ws-large-fields');
    const session = run.session();
    expect(session?.result?.result).toBe('passed');
    expect(session?.unreadable).toEqual([]);
    // The summary is authoritative: the final summary shows that an overlong command was left out (it is not
    // unreadable)
    const engine = session?.participants.find((p) => p.name === 'Engine');
    expect(engine?.omittedFields).toEqual(['command']);
    expect(engine?.state).toBe('ended');
  });

  it('says so when events come with a seq that went back, instead of dropping them silently', () => {
    const { run, socket } = open();
    play(socket, 'ws-pass');
    const last = run.session()?.lastSeq ?? 0;
    socket.send({
      type: 'event',
      stream_seq: 9999,
      run_id: run.session()?.id,
      event: { event: 'log', seq: last - 1, t_ms: 0, source: 'Engine', line: 'late' },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.notices().map((n) => [n.message, n.count])).toEqual([[STALE_EVENTS_NOTICE, 1]]);
  });

  it('ignores duplicate or out-of-order stream_seq frames after synced', () => {
    const { run, socket } = open();
    play(socket, 'ws-pass');
    const linesBefore = run.session()?.logs.length ?? 0;
    socket.send({
      type: 'event',
      stream_seq: 1,
      run_id: run.session()?.id,
      event: { event: 'log', seq: 9999, t_ms: 1000, source: 'Engine', line: 'duplicate frame' },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()?.logs).toHaveLength(linesBefore);
    expect(run.notices()).toEqual([]);
  });

  it('marks in the log where the server could not send lines, and counts them', () => {
    const { run, socket } = open();
    play(socket, 'ws-dropped');
    const session = run.session();
    const dropped = (examples.get('ws-dropped')?.server ?? []).find(
      (m) => (m as { type: string }).type === 'dropped',
    ) as { log: Record<string, number> };
    const count = dropped.log['Engine'];
    expect(count).toBeGreaterThan(0);
    expect(session?.logSources['Engine'].dropped).toBe(count);
    const gap = session?.logs.find((line) => line.gap);
    expect(gap?.source).toBe('Engine');
    expect(gap?.gap?.count).toBe(count);
    expect(gap?.tMs).toBeNull();
  });

  it('builds the state again after a resync', () => {
    const { run, socket } = open();
    play(socket, 'ws-pass', 0, 10);
    expect(run.ready()).toBe(true);
    play(socket, 'ws-resync');
    // Not in sync until everything is sent again from hello
    expect(run.ready()).toBe(false);
    play(socket, 'ws-replay');
    expect(run.ready()).toBe(true);
    expect(run.session()?.result?.result).toBe('passed');
  });
});
