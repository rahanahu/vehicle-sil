import { TestBed } from '@angular/core/testing';
import { readSessionResponse } from '../api';
import { AuthToken } from '../auth/auth-token';
import { FakeSocket, FakeWebSockets } from '../streams/fake-web-socket';
import { backoffMs } from '../streams/live-connection';
import { WebSocketExample, httpExamples, webSocketExamples } from '../../testing/protocol-examples';
import { BATCH_MS, STALE_EVENTS_NOTICE, ServerConnection } from './server-connection';
import { runOutcome } from './session-state';

describe('ServerConnection', () => {
  let fake: FakeWebSockets;
  let examples: Map<string, WebSocketExample>;

  beforeAll(async () => {
    examples = new Map((await webSocketExamples()).map((example) => [example.name, example]));
  });

  beforeEach(() => {
    vi.useFakeTimers();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    TestBed.configureTestingModule({
      providers: [fake.provider],
    });
  });

  afterEach(() => {
    vi.useRealTimers();
    sessionStorage.clear();
  });

  /** 例の server の通知を、届いた順にそのまま流す。 */
  function play(socket: FakeSocket, name: string, from = 0, to?: number) {
    const example = examples.get(name);
    if (!example) throw new Error(`no example ${name}`);
    for (const message of example.server.slice(from, to)) socket.send(message);
    vi.advanceTimersByTime(BATCH_MS);
  }

  function open(): { run: ServerConnection; socket: FakeSocket } {
    const run = TestBed.inject(ServerConnection);
    const socket = fake.last();
    socket.open();
    return { run, socket };
  }

  it('authenticates first, then follows a whole run', () => {
    const { run, socket } = open();
    expect(socket.greeting).toEqual([{ type: 'auth', token: 'secret' }]);
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
    expect(session?.logs.some((line) => line.source === 'Engine')).toBe(true);
    expect(session?.check?.result).toBe('passed');
  });

  it('builds the same state from the replay of a screen that connects later', () => {
    const live = open();
    play(live.socket, 'ws-pass');
    const expected = live.run.session();

    TestBed.resetTestingModule();
    TestBed.configureTestingModule({
      providers: [fake.provider],
    });
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
    // 実行の途中までを受け取ってから切れる
    play(socket, 'ws-replay', 0, 30);
    const before = run.session();
    expect(before?.lastSeq).toBeGreaterThan(0);
    socket.fail();
    vi.advanceTimersByTime(backoffMs(1));

    expect(run.ready()).toBe(false);
    const again = fake.last();
    expect(again).not.toBe(socket);
    expect(again.greeting).toEqual([
      { type: 'auth', token: 'secret', session: before?.id, from_seq: (before?.lastSeq ?? 0) + 1 },
    ]);
    again.open();
    // つながっても、再送を受け終えるまでは揃っていない
    expect(run.ready()).toBe(false);
    // 続きの通知だけが届く（hello の要約と、from_seq からの事象）
    again.send({
      ...(replay?.server[0] as object),
      replay: { from_seq: (before?.lastSeq ?? 0) + 1, omitted: { log: {}, events: 0 } },
    });
    play(again, 'ws-replay', 30);
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
    expect(TestBed.inject(AuthToken).rejected()).toBe(true);
    socket.close();
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
    expect(run.session()).toBeNull();
  });

  it('takes the summary of an answer at once, before the notification', async () => {
    const { run, socket } = open();
    play(socket, 'ws-pass', 0, 2);
    const answer = (await httpExamples()).find((example) => example.name === 'run');
    run.adopt(readSessionResponse(answer?.response));
    expect(run.session()?.id).toBe(readSessionResponse(answer?.response)?.id);
    expect(run.session()?.launcher.running).toBe(true);
  });

  it('waits longer each time the server says to try again later, and does not take it as a refused token', () => {
    const { run, socket } = open();
    socket.close(1013, 'too-many-connections');
    expect(run.tryAgainReason()).toBe('too-many-connections');
    expect(TestBed.inject(AuthToken).rejected()).toBe(false);
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);

    fake.last().open();
    fake.last().close(1013, 'too-many-unauthenticated');
    expect(run.tryAgainReason()).toBe('too-many-unauthenticated');
    vi.advanceTimersByTime(backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);

    // つながり直して通知が届けば、理由は消え、次の待ち時間は最初に戻る
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
    const answer = (name: string) => readSessionResponse(answers.get(name)?.response);
    // 新しい実行（2）の要約が先に届き、停止の応答は前の実行（1）の要約か null
    const started = answer('run');
    if (!started) throw new Error('no summary in run.json');
    run.adopt({ ...started, id: 2 });
    run.adopt(answer('session-ended'));
    run.adopt(null);
    expect(run.session()?.id).toBe(2);
    expect(run.session()?.launcher.running).toBe(true);
    // 始まった実行の要約に、届けた停止が出る
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
    // 間を置いてつなぎ直し、読める hello から揃う
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
    // 開いていた画面に、同じ要約が session で届いても同じ
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
    // 要約が正: 終わりの要約で、長すぎた command を省いたことが出る（読めないのではない）
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
    // 停止の応答（実行中の要約）が、終わりの通知より遅れて届いた
    const answers = new Map((await httpExamples()).map((example) => [example.name, example]));
    const stop = readSessionResponse(answers.get('stop')?.response);
    if (!stop) throw new Error('no summary in stop.json');
    run.adopt({ ...stop, id: ended?.id ?? 1, last_seq: 10_000 });
    expect(run.session()?.launcher.running).toBe(false);
    expect(run.session()?.findings).toEqual(ended?.findings);
  });

  it('marks in the log where the server could not send lines, and counts them', () => {
    const { run, socket } = open();
    play(socket, 'ws-dropped');
    const session = run.session();
    // ws-dropped.json の dropped の数（例を作り直すと変わるので、例から読む）
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
    // hello から送り直されるまでは、揃っていない
    expect(run.ready()).toBe(false);
    play(socket, 'ws-replay');
    expect(run.ready()).toBe(true);
    expect(run.session()?.result?.result).toBe('passed');
  });
});
