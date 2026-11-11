import { provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { AuthToken } from '../core/auth/auth-token';
import { FakeSocket, FakeWebSockets } from '../core/streams/fake-web-socket';
import { backoffMs } from '../core/streams/live-connection';
import { WebSocketExample, webSocketExamples } from '../testing/protocol-examples';
import { BATCH_MS, RunSession } from './run-session';
import { isEnded } from './run-state';

describe('RunSession', () => {
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
      providers: [RunSession, provideHttpClient(), provideHttpClientTesting(), fake.provider],
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

  function open(): { run: RunSession; socket: FakeSocket } {
    const run = TestBed.inject(RunSession);
    const socket = fake.last();
    socket.open();
    return { run, socket };
  }

  it('authenticates first, then follows a whole run', () => {
    const { run, socket } = open();
    expect(socket.greeting).toEqual([{ type: 'auth', token: 'secret' }]);
    expect(run.session()).toBeNull();

    play(socket, 'ws-pass');
    const session = run.session();
    expect(session?.file).toBe('simulation.yaml');
    expect(session?.result?.result).toBe('passed');
    expect(session?.phase).toBe('ended');
    expect(run.active()).toBe(false);
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
      providers: [RunSession, provideHttpClient(), provideHttpClientTesting(), fake.provider],
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

    const again = fake.last();
    expect(again).not.toBe(socket);
    expect(again.greeting).toEqual([
      { type: 'auth', token: 'secret', session: before?.id, from_seq: (before?.lastSeq ?? 0) + 1 },
    ]);
    again.open();
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
  });

  it('ends a run whose launcher died without a result, and keeps what the server noticed', () => {
    const { run, socket } = open();
    play(socket, 'ws-launcher-crash');
    const session = run.session();
    expect(session && isEnded(session)).toBe(true);
    expect(session?.result).toBeNull();
    expect(session?.problems.map((p) => p.code)).toContain('no-result');
    expect(run.active()).toBe(false);
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

  it('starts and stops through the API, and shows why the server refused', () => {
    const run = TestBed.inject(RunSession);
    const http = TestBed.inject(HttpTestingController);
    run.start({ file: 'a.yaml', disable: ['B'], speed: 'max', start_paused: false });
    expect(run.pending()).toBe('start');
    const request = http.expectOne('api/run');
    expect(request.request.body).toEqual({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
      start_paused: false,
    });
    request.flush(
      { error: { code: 'session-running', message: 'a simulation is running', detail: null } },
      { status: 409, statusText: 'Conflict' },
    );
    expect(run.pending()).toBeNull();
    expect(run.actionError()).toBe('a simulation is running');

    run.stop();
    http.expectOne('api/stop').flush({ session: null });
    expect(run.actionError()).toBeNull();
    http.verify();
  });
});
