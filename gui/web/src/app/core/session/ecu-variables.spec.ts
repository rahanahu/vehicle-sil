import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import {
  EnvironmentInjector,
  createEnvironmentInjector,
  runInInjectionContext,
} from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { BATCH_MS, WriteError } from '@vehicle-sil/gui-client';
import { FakeWebSockets, httpExamples, webSocketExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../auth/auth-interceptor';
import { EcuVariables } from './ecu-variables';
import { SOCKET_FACTORY } from './gui-client';

const DEMAND = { peer: 'Engine', name: 'engine.demand' };

// 包みの試験: 状態と値が signal に写ること、部品が壊れると watch をやめること、書き込みが HttpClient で出ること。
// まとめ方、つなぎ直し、履歴、結果の待ち方は gui/client の試験（variables.spec.ts）。
describe('EcuVariables', () => {
  let fake: FakeWebSockets;
  let http: HttpTestingController;
  let server: Map<string, unknown[]>;

  beforeAll(async () => {
    server = new Map((await webSocketExamples()).map((e) => [e.name, e.server]));
  });

  beforeEach(() => {
    vi.useFakeTimers();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
      ],
    });
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => {
    vi.useRealTimers();
    sessionStorage.clear();
  });

  const sent = () => fake.sockets[0].sent.filter((m) => (m as { type: string }).type !== 'auth');

  it('shows the schema and the values as signals, and stops watching with the component', async () => {
    const variables = TestBed.inject(EcuVariables);
    expect(variables.status()).toBe('unknown');
    // 部品の代わり。destroy() で、部品が壊れたときと同じく watch が終わる
    const component = createEnvironmentInjector([], TestBed.inject(EnvironmentInjector));
    const watch = runInInjectionContext(component, () =>
      variables.watch([DEMAND], { periodMs: 100, history: 10 }),
    );
    fake.sockets[0].open();
    for (const message of server.get('ws-variables') ?? []) fake.sockets[0].send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
    expect(variables.status()).toBe('ended');
    expect(variables.peers().map((peer) => peer.name)).toEqual(['Engine', 'Dashboard']);
    expect(variables.find(DEMAND)).toMatchObject({ type: 'real', unit: '%', min: 0, max: 100 });
    expect(variables.find({ peer: 'Engine', name: 'nothing' })).toBeUndefined();
    expect(watch.value(DEMAND)).toEqual({ value: 40, sampleMs: 200, simMs: 200, paused: true });
    expect(watch.values()['Engine']?.['engine.demand']?.value).toBe(40);
    expect(watch.history(DEMAND)).toEqual([{ sampleMs: 200, value: 40 }]);
    expect(variables.operations().map((o) => [o.seq, o.state])).toEqual([[1, 'applied']]);
    expect(sent()).toEqual([{ type: 'watch', id: 1, variables: [DEMAND], period_ms: 100 }]);
    component.destroy();
    await vi.advanceTimersByTimeAsync(0);
    expect(sent()[1]).toEqual({ type: 'unwatch', id: 1 });
  });

  it('writes through HttpClient with the token, and passes on why a write was refused', async () => {
    const variables = TestBed.inject(EcuVariables);
    const refused = (await httpExamples()).find((e) => e.name === 'error-not-writable');
    // 書き込みは、いまの実行（ここでは 1）に向ける
    fake.sockets[0].open();
    for (const message of server.get('ws-variables') ?? []) fake.sockets[0].send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
    const write = variables.write({ Engine: { 'engine.speed': 900 } }).catch((e: unknown) => e);
    const request = http.expectOne('api/write');
    expect(request.request.headers.get('Authorization')).toBe('Bearer secret');
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'engine.speed': 900 } },
    });
    request.flush(refused?.response ?? null, { status: 400, statusText: 'Bad Request' });
    const error = await write;
    expect(error).toBeInstanceOf(WriteError);
    expect(error).toMatchObject({ kind: 'refused', code: 'not-writable' });
    http.verify();
  });

  it('makes no watch when it is called outside an injection context', async () => {
    const variables = TestBed.inject(EcuVariables);
    fake.sockets[0].open();
    expect(() => variables.watch([DEMAND])).toThrow();
    await vi.advanceTimersByTimeAsync(0);
    expect(sent()).toEqual([]); // 作っていないので、購読も送らない
  });
});
