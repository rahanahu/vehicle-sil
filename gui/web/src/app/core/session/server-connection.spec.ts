import { TestBed } from '@angular/core/testing';
import { BATCH_MS, ServerMessage } from '@vehicle-sil/gui-client';
import { backoffMs } from '@vehicle-sil/gui-client/internal';
import {
  FakeWebSockets,
  WebSocketExample,
  webSocketExamples,
} from '@vehicle-sil/gui-client/testing';
import { AuthToken } from '../auth/auth-token';
import { SOCKET_FACTORY } from './gui-client';
import { ServerConnection } from './server-connection';

// 包みの試験: ライブラリの状態が signal に写ること、トークンの扱い、寿命。要約と事象の当て方は gui/client の試験。
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
      providers: [{ provide: SOCKET_FACTORY, useValue: fake.factory }],
    });
  });

  afterEach(() => {
    vi.useRealTimers();
    sessionStorage.clear();
  });

  function play(name: string) {
    for (const message of examples.get(name)?.server ?? []) fake.last().send(message);
    vi.advanceTimersByTime(BATCH_MS);
  }

  it('connects with the token of the page, and shows the run as signals', () => {
    const run = TestBed.inject(ServerConnection);
    expect(fake.sockets).toHaveLength(1);
    expect(fake.last().url).toMatch(/^ws:\/\/.*\/ws$/);
    expect(run.connectionState()).toBe('connecting');
    fake.last().open();
    expect(fake.last().greeting).toEqual([
      { type: 'auth', token: 'secret', session_without_check: true },
    ]);
    expect(run.connectionState()).toBe('open');
    expect(run.ready()).toBe(false);

    play('ws-pass');
    expect(run.ready()).toBe(true);
    expect(run.session()?.result?.result).toBe('passed');
    expect(run.lastEventAt()).toBeGreaterThan(0);
  });

  it('hands the batches it read to whoever subscribes', () => {
    const run = TestBed.inject(ServerConnection);
    const types: string[] = [];
    const subscription = run.batches.subscribe((batch: readonly ServerMessage[]) =>
      types.push(...batch.map((m) => m.type)),
    );
    fake.last().open();
    play('ws-pass');
    subscription.unsubscribe();
    expect(types.slice(0, 2)).toEqual(['hello', 'synced']);
  });

  it('says why it does not connect when the server speaks another version', () => {
    const run = TestBed.inject(ServerConnection);
    fake.last().open();
    const hello = examples.get('ws-replay')?.server[0] as Record<string, unknown>;
    fake.last().send({ ...hello, vehicle_sil_gui: 2 });
    vi.advanceTimersByTime(BATCH_MS);
    expect(run.incompatible()).toContain('version 2');
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
  });

  it('marks the token refused when the WebSocket refuses it, and stops reconnecting', () => {
    const run = TestBed.inject(ServerConnection);
    fake.last().open();
    play('ws-unauthorized');
    expect(TestBed.inject(AuthToken).rejected()).toBe(true);
    fake.last().close(4401, 'unauthorized');
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
    expect(run.session()).toBeNull();
  });

  it('does not connect without a token', () => {
    sessionStorage.clear();
    TestBed.inject(ServerConnection);
    expect(fake.sockets).toHaveLength(0);
  });

  it('closes the socket when the app goes away', () => {
    TestBed.inject(ServerConnection);
    fake.last().open();
    TestBed.resetTestingModule();
    expect(fake.last().closedByClient).toBe(true);
  });
});
