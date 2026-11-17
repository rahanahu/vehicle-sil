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
      http.expectOne('api/session').flush({ session: summary });
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
      allowed: { rev: 99, ...allowed, stop: allowed.start, force: allowed.start },
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
    socket.send({ ...hello, vehicle_sil_gui: 2 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(socket.closedByClient).toBe(true);
    expect(run.ready()).toBe(false);
    expect(run.incompatible()).toContain('version 2');
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

  it('asks for the rest after a reconnection, and keeps what it has', () => {
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
        session: before?.id,
        from_seq: (before?.lastSeq ?? 0) + 1,
        session_without_check: true,
      },
    ]);
    // Even when connected, not in sync until the resend has arrived
    expect(run.ready()).toBe(false);
    // Only the continuation arrives (the hello summary, and the events from from_seq)
    again.send({
      ...(replay?.server[0] as object),
      replay: { from_seq: (before?.lastSeq ?? 0) + 1, omitted: { log: {}, events: 0 } },
    });
    play(again, 'ws-replay', half);
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
