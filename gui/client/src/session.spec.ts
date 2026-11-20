import { GuiClient, createClient } from './client';
import { BATCH_MS, backoffMs } from './connection';
import { readSessionResponse } from './protocol';
import { STALE_EVENTS_NOTICE } from './session';
import { runOutcome } from './session-state';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { WebSocketExample, httpExamples, webSocketExamples } from './testing/protocol-examples';

/** What the tests read (the readable values of the session copy, as functions that return the value). */
function view(client: GuiClient, http: FakeHttp) {
  const feed = client.session;
  return {
    session: () => feed.session.get(),
    ready: () => feed.ready.get(),
    notices: () => feed.notices.get(),
    rejected: () => feed.rejected.get(),
    tryAgainReason: () => client.connection.tryAgainReason.get(),
    incompatible: () => feed.incompatible.get(),
    /** Applies the summary of an HTTP answer (in the server's shape), as the answer to GET api/session. */
    adopt: (summary: unknown) => {
      void client.fetchSession().catch(() => undefined);
      http.expectOne('api/session').flush({ instance: '0123456789abcdef', session: summary });
    },
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
    return { run: view(client, http), socket };
  }

  it('exposes the authenticated instance even when no run exists', () => {
    const { socket } = open();
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'new-server',
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
    expect(socket.greeting).toEqual([
      { type: 'auth', token: 'secret', session_without_check: true },
    ]);
    expect(run.session()).toBeNull();
    expect(run.ready()).toBe(false);

    play(socket, 'ws-pass');
    expect(run.ready()).toBe(true);
    const session = run.session();
    expect(session?.file).toBe('simulation.yaml');
    expect(session?.result?.result).toBe('passed');
    expect(session?.phase).toBe('ended');
    expect(session && runOutcome(session)).toBe('ended');
    expect(session?.participants.map((p) => [p.name, p.state])).toContainEqual(['Engine', 'ended']);
    // In the example ECUs, Dashboard writes the lines
    expect(session?.logs.some((line) => line.source === 'Dashboard')).toBe(true);
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
      session: null,
      allowed: {
        rev: 99,
        ...allowed,
        stop: allowed.start,
        force: allowed.start,
        save: { ...allowed.start, caution: null },
      },
    });
    socket.send({ type: 'a-message-of-a-later-version', session: 1 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.session()).toBe(before); // the same session copy: nothing changed
    expect(seen).toEqual(['allowed']); // the one it knows is passed on, the unknown one is skipped
  });

  it('keeps the check of the hello when the later summaries come without it', () => {
    const { run, socket } = open();
    // The connected screen asks for session_without_check: the session summaries have no check field
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

  it('stops and says why when the server speaks another version', () => {
    const { run, socket } = open();
    const hello = examples.get('ws-replay')?.server[0] as Record<string, unknown>;
    socket.send({ ...hello, vehicle_sil_gui: 3 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(socket.closedByClient).toBe(true);
    expect(run.ready()).toBe(false);
    expect(run.incompatible()).toContain('version 3');
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

  it('builds the same state from the replay of a screen that connects later', () => {
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

  it('replays from the beginning after a reconnection, even if a server reused the id', () => {
    const { run, socket } = open();
    const replay = examples.get('ws-replay');
    const half = Math.floor((replay?.server.length ?? 0) / 2);
    // The connection drops after receiving part of the run
    play(socket, 'ws-replay', 0, half);
    const before = run.session();
    expect(before?.lastSeq).toBeGreaterThan(0);
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
        session_without_check: true,
      },
    ]);
    // Even when connected, not in sync until the resend has arrived
    expect(run.ready()).toBe(false);
    // A full replay does not rely on a cursor from the previous server instance.
    play(again, 'ws-replay');
    const logs = (replay?.server ?? []).filter(
      (message) => (message as { event?: { event: string } }).event?.event === 'log',
    );
    expect(run.session()?.logs).toHaveLength(logs.length);
    expect(run.session()?.logs[0]).toEqual(before?.logs[0]);
    expect(run.session()?.result?.result).toBe('passed');
    expect(run.ready()).toBe(true);
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

  it('takes the summary of an answer at once, before the notification', async () => {
    const { run, socket } = open();
    play(socket, 'ws-pass', 0, 2);
    const answer = (await httpExamples()).find((example) => example.name === 'run');
    run.adopt((answer?.response as { session: unknown }).session);
    expect(run.session()?.id).toBe(readSessionResponse(answer?.response)?.id);
    expect(run.session()?.launcher.running).toBe(true);
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

  it('keeps a run asked to stop while it started: the answer of the run before does not replace it', async () => {
    const { run, socket } = open();
    play(socket, 'ws-pass', 0, 2);
    const answers = new Map((await httpExamples()).map((example) => [example.name, example]));
    const answer = (name: string) =>
      (answers.get(name)?.response as { session: Record<string, unknown> | null }).session;
    // The new run's (2) summary arrives first; the stop's answer is the previous run's (1) summary or null
    const started = answer('run');
    if (!started) throw new Error('no summary in run.json');
    run.adopt({ ...started, id: 2 });
    run.adopt(answer('session-ended'));
    run.adopt(null);
    expect(run.session()?.id).toBe(2);
    expect(run.session()?.launcher.running).toBe(true);
    // The summary of the run that started shows the stop that was delivered
    const stopped = (answers.get('stop')?.response as { session: { stop: string } }).session;
    expect(stopped.stop).toBe('requested');
    socket.send({ type: 'session', session: { ...stopped, id: 2 } });
    vi.advanceTimersByTime(BATCH_MS);
    const session = run.session();
    expect(session?.id).toBe(2);
    expect(session && runOutcome(session)).toBe('stopping');
  });

  it('counts a notice that comes again instead of repeating it', () => {
    const { run, socket } = open();
    for (let i = 0; i < 3; i++) socket.send({ type: 'event', session: 1 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.notices()).toHaveLength(1);
    expect(run.notices()[0].count).toBe(3);
  });

  it('flushes the old socket before close subscribers adopt a new server', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    let switched = false;
    const unsubscribe = client.connection.state.subscribe((state) => {
      if (state !== 'closed' || switched) return;
      switched = true;
      void client.fetchSession();
      http
        .expectOne('api/session')
        .flush({ instance: 'new-server', session: { ...response.session, file: 'new.yaml' } });
    });
    socket.send({ type: 'session', session: { ...response.session, file: 'old.yaml' } });
    socket.fail();
    vi.advanceTimersByTime(BATCH_MS);
    unsubscribe();
    expect(switched).toBe(true);
    expect(client.session.session.get()?.file).toBe('new.yaml');
    expect(client.session.currentTarget()).toEqual({
      instance: 'new-server',
      session: response.session['id'],
    });
  });

  it('keeps a newer HTTP launcher snapshot over an older buffered hello and preserves replay', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const launcher = response.session['launcher'] as Record<string, unknown>;
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: 1 },
      replay: null,
    });
    socket.send({ type: 'synced', session: response.session['id'], last_seq: 0 });
    const refresh = client.fetchSession();
    http.expectOne('api/session').flush({
      instance: 'x',
      session: {
        ...response.session,
        snapshot_rev: 2,
        launcher: { ...launcher, running: false },
        state: 'ended',
      },
    });
    await refresh;
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.launcher.running).toBe(false);
    expect(client.session.session.get()?.snapshotRev).toBe(2);
    expect(client.session.ready.get()).toBe(true);
  });

  it('keeps a complete live result when the first HTTP summary arrives before hello', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const id = response.session['id'];
    const result = {
      vehicle_sil_run: 2,
      result: 'passed',
      end: 'completed',
      ok: true,
      message: 'x'.repeat(4 * 1024 * 1024 + 1),
      errors: 0,
      warnings: 0,
      notes: 0,
      participants: [
        {
          name: 'Engine',
          required: true,
          enabled: true,
          manual: false,
          ended: 'exited',
          exit_code: 0,
          signal: null,
        },
      ],
      findings: [],
    };
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: 1 },
      replay: null,
    });
    socket.send({
      type: 'event',
      session: id,
      event: { event: 'result', seq: 1, t_ms: 1, result },
    });
    socket.send({ type: 'synced', session: id, last_seq: 1 });
    const refresh = client.fetchSession();
    http.expectOne('api/session').flush({
      instance: 'x',
      session: {
        ...response.session,
        snapshot_rev: 2,
        last_seq: 1,
        result: {
          vehicle_sil_run: 2,
          result: 'passed',
          end: 'completed',
          ok: true,
          errors: 0,
          warnings: 0,
          notes: 0,
        },
      },
    });
    await refresh;
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.result?.participants[0]?.name).toBe('Engine');
    expect(client.session.session.get()?.result?.message.length).toBe(result.message.length);
  });

  it('deduplicates a buffered finding already in the first HTTP summary', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const id = response.session['id'];
    const finding = {
      severity: 'warning',
      code: 'x',
      message: 'one',
      file: null,
      line: null,
      participant: null,
      network: null,
      detail: null,
    };
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: 1, findings: [] },
      replay: null,
    });
    socket.send({
      type: 'event',
      session: id,
      event: { event: 'finding', seq: 1, t_ms: 1, ...finding },
    });
    socket.send({ type: 'synced', session: id, last_seq: 1 });
    const refresh = client.fetchSession();
    http.expectOne('api/session').flush({
      instance: 'x',
      session: { ...response.session, snapshot_rev: 2, last_seq: 1, findings: [finding] },
    });
    await refresh;
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.findings.map((item) => item.message)).toEqual(['one']);
  });

  it('retains complete large result and check across a same-run full hello replay', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const examples = await httpExamples();
    const response = examples.find((item) => item.name === 'run')?.response as {
      session: Record<string, unknown>;
    };
    const checked = examples.find((item) => item.name === 'check-passed')?.response as {
      check: Record<string, unknown>;
    };
    const id = response.session['id'];
    const check = { ...checked.check, padding: 'x'.repeat(4 * 1024 * 1024 + 1) };
    const result = {
      vehicle_sil_run: 2,
      result: 'passed',
      end: 'completed',
      ok: true,
      message: 'x'.repeat(4 * 1024 * 1024 + 1),
      errors: 0,
      warnings: 0,
      notes: 0,
      participants: [
        {
          name: 'Engine',
          required: true,
          enabled: true,
          manual: false,
          ended: 'exited',
          exit_code: 0,
          signal: null,
        },
      ],
      findings: [],
    };
    const hello = (session: Record<string, unknown>) => ({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session,
      replay: { from_seq: null },
    });
    socket.send(hello({ ...response.session, snapshot_rev: 1 }));
    socket.send({ type: 'event', session: id, event: { event: 'check', seq: 1, t_ms: 1, check } });
    socket.send({
      type: 'event',
      session: id,
      event: { event: 'result', seq: 2, t_ms: 2, result },
    });
    socket.send({ type: 'synced', session: id, last_seq: 2 });
    vi.advanceTimersByTime(BATCH_MS);
    socket.send(
      hello({
        ...response.session,
        snapshot_rev: 2,
        last_seq: 2,
        check: null,
        result: {
          vehicle_sil_run: 2,
          result: 'passed',
          end: 'completed',
          ok: true,
          errors: 0,
          warnings: 0,
          notes: 0,
        },
      }),
    );
    socket.send({ type: 'synced', session: id, last_seq: 2 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.result?.participants[0]?.name).toBe('Engine');
    expect(client.session.session.get()?.result?.message.length).toBe(result.message.length);
    expect(client.session.session.get()?.check?.participants[0]?.name).toBe('Engine');
  });

  it('rebuilds all replayed logs after a stale hello without duplicating gaps', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const id = response.session['id'];
    const hello = (rev: number) => ({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: rev },
      replay: { from_seq: null },
    });
    const log = (seq: number) => ({
      type: 'event',
      session: id,
      event: { event: 'log', seq, t_ms: seq, source: 'ECU', line: `line ${seq}` },
    });
    const dropped = {
      type: 'dropped',
      session: id,
      reason: 'not-kept',
      log: { ECU: 1 },
      at_least: [],
      events: 0,
    };
    socket.send(hello(1));
    socket.send(dropped);
    socket.send(log(1));
    socket.send(log(3));
    socket.send({ type: 'synced', session: id, last_seq: 3 });
    vi.advanceTimersByTime(BATCH_MS);
    const refresh = client.fetchSession();
    http
      .expectOne('api/session')
      .flush({ instance: 'x', session: { ...response.session, snapshot_rev: 3 } });
    await refresh;
    socket.send(hello(2));
    socket.send(dropped);
    socket.send(log(1));
    socket.send(log(2));
    socket.send(log(3));
    socket.send({ type: 'synced', session: id, last_seq: 3 });
    vi.advanceTimersByTime(BATCH_MS);
    const logs = client.session.session.get()?.logs ?? [];
    expect(logs.filter((line) => !line.gap).map((line) => line.line)).toEqual([
      'line 1',
      'line 2',
      'line 3',
    ]);
    expect(logs.filter((line) => line.gap)).toHaveLength(1);
  });

  it('deduplicates a problem already included in a newer HTTP summary', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const id = response.session['id'];
    const problem = { code: 'stop-not-delivered', message: 'failed', detail: null, problem_seq: 1 };
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: 1 },
      replay: null,
    });
    vi.advanceTimersByTime(BATCH_MS);
    const refresh = client.fetchSession();
    http.expectOne('api/session').flush({
      instance: 'x',
      session: { ...response.session, snapshot_rev: 2, problem_seq: 1, problems: [problem] },
    });
    await refresh;
    socket.send({ type: 'error', session: id, ...problem });
    socket.send({ type: 'session', session: { ...response.session, snapshot_rev: 1 } });
    vi.advanceTimersByTime(BATCH_MS);
    expect(
      client.session.session.get()?.problems.filter((p) => p.code === problem.code),
    ).toHaveLength(1);
  });

  it('keeps diagnostics omitted from a summary when their websocket errors arrive later', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const id = response.session['id'];
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: { ...response.session, snapshot_rev: 1 },
      replay: null,
    });
    vi.advanceTimersByTime(BATCH_MS);
    const problems = Array.from({ length: 100 }, (_, index) => ({
      code: 'bad-event',
      message: `problem ${index + 1}`,
      detail: null,
      problem_seq: index + 1,
    }));
    const refresh = client.fetchSession();
    http.expectOne('api/session').flush({
      instance: 'x',
      session: {
        ...response.session,
        snapshot_rev: 2,
        problem_seq: 101,
        problems,
        problems_omitted: 1,
      },
    });
    await refresh;
    socket.send({
      type: 'error',
      session: id,
      code: 'bad-event',
      message: 'problem 101',
      detail: null,
      problem_seq: 101,
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.problems).toHaveLength(101);
    expect(client.session.session.get()?.problemsOmitted).toBe(0);
    socket.send({
      type: 'session',
      session: {
        ...response.session,
        snapshot_rev: 3,
        problem_seq: 101,
        problems,
        problems_omitted: 1,
      },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.problems).toHaveLength(101);
    expect(client.session.session.get()?.problemsOmitted).toBe(0);
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 2,
      version: '0.1.0',
      instance: 'x',
      session: {
        ...response.session,
        snapshot_rev: 4,
        problem_seq: 101,
        problems,
        problems_omitted: 1,
      },
      replay: { from_seq: null },
    });
    socket.send({ type: 'synced', session: id, last_seq: 0 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.problems).toHaveLength(101);
    expect(client.session.session.get()?.problemsOmitted).toBe(0);
  });

  it.each(['old-first', 'new-first'] as const)(
    'orders overlapping same-run session snapshots by server revision (%s)',
    async (order) => {
      const http = new FakeHttp();
      open(http);
      const client = clientsLast();
      const example = (await httpExamples()).find((item) => item.name === 'run');
      const response = example?.response as { session: Record<string, unknown> };
      const launcher = response.session['launcher'] as Record<string, unknown>;
      const old = client.fetchSession();
      const oldRequest = http.expectOne('api/session');
      const newer = client.fetchSession();
      const newRequest = http.expectOne('api/session');
      const answerOld = () =>
        oldRequest.flush({ instance: 'x', session: { ...response.session, snapshot_rev: 1 } });
      const answerNew = () =>
        newRequest.flush({
          instance: 'x',
          session: {
            ...response.session,
            snapshot_rev: 2,
            launcher: { ...launcher, running: false },
            state: 'ended',
          },
        });
      if (order === 'old-first') {
        answerOld();
        answerNew();
      } else {
        answerNew();
        answerOld();
      }
      await Promise.all([old, newer]);
      expect(client.session.session.get()?.launcher.running).toBe(false);
      expect(client.session.session.get()?.snapshotRev).toBe(2);
    },
  );

  it('does not apply an old prefix to a new server during restart notification', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    let switched = false;
    const unsubscribe = client.connection.state.subscribe((state) => {
      if (state !== 'closed' || switched) return;
      switched = true;
      void client.fetchSession();
      http
        .expectOne('api/session')
        .flush({ instance: 'new-server', session: { ...response.session, file: 'new.yaml' } });
    });
    socket.send({ type: 'session', session: { ...response.session, file: 'old.yaml' } });
    socket.send({ type: 'session', session: { id: 'invalid' } });
    vi.advanceTimersByTime(BATCH_MS);
    unsubscribe();
    expect(switched).toBe(true);
    expect(client.session.currentInstance()).toBe('new-server');
    expect(client.session.session.get()?.file).toBe('new.yaml');
    expect(client.session.currentTarget()).toEqual({
      instance: 'new-server',
      session: response.session['id'],
    });
  });

  it('does not restart or apply the old prefix after a notice switches server', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    let switched = false;
    const unsubscribe = client.session.notices.subscribe(() => {
      if (switched) return;
      switched = true;
      socket.fail();
      void client.fetchSession();
      http
        .expectOne('api/session')
        .flush({ instance: 'new-server', session: { ...response.session, file: 'new.yaml' } });
    });
    socket.send({ type: 'session', session: { ...response.session, file: 'old.yaml' } });
    socket.send({ type: 'session', session: { id: 'invalid' } });
    vi.advanceTimersByTime(BATCH_MS);
    unsubscribe();
    expect(switched).toBe(true);
    expect(client.session.session.get()?.file).toBe('new.yaml');
    expect(client.session.currentTarget()?.instance).toBe('new-server');
  });

  it('filters an older prefix if restart subscribers fetch a newer run on the same server', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    let switched = false;
    const unsubscribe = client.connection.state.subscribe((state) => {
      if (state !== 'closed' || switched) return;
      switched = true;
      void client.fetchSession();
      http.expectOne('api/session').flush({
        instance: '0123456789abcdef',
        session: { ...response.session, id: 2, file: 'new.yaml' },
      });
    });
    socket.send({ type: 'session', session: { ...response.session, id: 1, file: 'old.yaml' } });
    socket.send({ type: 'session', session: { id: 'invalid' } });
    vi.advanceTimersByTime(BATCH_MS);
    unsubscribe();
    expect(switched).toBe(true);
    expect(client.session.session.get()?.file).toBe('new.yaml');
    expect(client.session.currentTarget()).toEqual({ instance: '0123456789abcdef', session: 2 });
  });

  it('keeps a valid prefix when a later malformed message restarts the same connection', async () => {
    const http = new FakeHttp();
    const { socket } = open(http);
    play(socket, 'ws-pass', 0, 2);
    const client = clientsLast();
    const example = (await httpExamples()).find((item) => item.name === 'run');
    const response = example?.response as { session: Record<string, unknown> };
    const received: string[] = [];
    client.session.onMessages((batch) => received.push(...batch.map((message) => message.type)));
    socket.send({ type: 'session', session: { ...response.session, file: 'prefix.yaml' } });
    socket.send({ type: 'session', session: { id: 'invalid' } });
    vi.advanceTimersByTime(BATCH_MS);
    expect(client.session.session.get()?.file).toBe('prefix.yaml');
    expect(received).toContain('session');
  });

  it('does not take an unreadable hello as in step, and connects again', () => {
    const { run, socket } = open();
    const hello = examples.get('ws-replay')?.server[0] as Record<string, unknown>;
    socket.send({ ...hello, session: { id: 'one' } });
    socket.send({ type: 'synced', session: null, last_seq: null });
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
    // The same summary arriving as session on a screen that was open changes nothing
    const hello = examples.get('ws-kept-limit')?.server[0] as { session: object };
    const live = open();
    play(live.socket, 'ws-pass', 0, 2);
    live.socket.send({ type: 'session', session: hello.session });
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
    expect(session?.logs.some((line) => line.line === 'a log event without t_ms')).toBe(true);
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
      session: run.session()?.id,
      event: { event: 'log', seq: last - 1, t_ms: 0, source: 'Engine', line: 'late' },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.notices().map((n) => [n.message, n.count])).toEqual([[STALE_EVENTS_NOTICE, 1]]);
  });

  it('does not take an older answer as running again, nor repeat findings that the notifications bring', async () => {
    const { run, socket } = open();
    play(socket, 'ws-pass');
    const ended = run.session();
    expect(ended?.launcher.running).toBe(false);
    // The stop's answer (a summary from during the run) arrived later than the end notification
    const answers = new Map((await httpExamples()).map((example) => [example.name, example]));
    const stop = (answers.get('stop')?.response as { session: Record<string, unknown> | null })
      .session;
    if (!stop) throw new Error('no summary in stop.json');
    run.adopt({ ...stop, id: ended?.id ?? 1, last_seq: 10_000 });
    expect(run.session()?.launcher.running).toBe(false);
    expect(run.session()?.findings).toEqual(ended?.findings);
  });

  it('marks in the log where the server could not send lines, and counts them', () => {
    const { run, socket } = open();
    play(socket, 'ws-dropped');
    const session = run.session();
    // The dropped counts of ws-dropped.json (read from the example, since they change when the examples are
    // regenerated)
    const dropped = (examples.get('ws-dropped')?.server ?? []).find(
      (m) => (m as { type: string }).type === 'dropped',
    ) as { log: Record<string, number> };
    const count = dropped.log['Engine'];
    expect(count).toBeGreaterThan(0);
    expect(session?.logSources['Engine'].dropped).toBe(count);
    const gap = session?.logs.find((line) => line.gap);
    expect(gap?.source).toBe('Engine');
    expect(gap?.gap?.count).toBe(count);
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
