import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { FakeWebSockets } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { ServerConnection } from '../core/session/server-connection';
import { provideVisibleNow } from '../testing/visible-now';
import { DashboardSpec } from './dashboard-spec';
import { DashboardView } from './dashboard-view';

/**
 * 偽の IntersectionObserver: 部品を作る時（@defer の viewport）を試験が決める。本物は描画の後に非同期で知らせ、
 * いつ知らせるかを決められない。値の watch の見え方（Visibility）は VisibleNow に替えてあるので、これを使うのは
 * @defer だけ。
 */
class FakeObserver {
  static last: FakeObserver | null = null;
  readonly observed = new Set<Element>();
  constructor(
    private readonly callback: (entries: { target: Element; isIntersecting: boolean }[]) => void,
  ) {
    FakeObserver.last = this;
  }
  observe(element: Element) {
    this.observed.add(element);
  }
  unobserve(element: Element) {
    this.observed.delete(element);
  }
  disconnect() {
    this.observed.clear();
  }
  tell(target: Element, isIntersecting: boolean) {
    this.callback([{ target, isIntersecting }]);
  }
}

const variable = (name: string, fields: object = {}) => ({
  name,
  type: 'real',
  role: 'output',
  writable: false,
  builtin: false,
  unit: '',
  min: null,
  max: null,
  step: null,
  description: '',
  enum: null,
  ...fields,
});

const hello = {
  type: 'hello',
  vehicle_sil_gui: 4,
  version: '0.1.0',
  instance: 'x',
  session: {
    id: '1',
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: {
      state: 'ready',
      reason: null,
      peers: [
        {
          name: 'Engine',
          connected: true,
          variables: [variable('engine.speed'), variable('engine.load'), variable('engine.temp')],
        },
      ],
      peers_omitted: 0,
      variables_omitted: 0,
      operations: [],
    },
  },
  replay: null,
};

const SPEC: DashboardSpec = {
  sections: [
    {
      title: 'Engine',
      items: [
        { peer: 'Engine', name: 'engine.speed', widget: 'value' },
        { peer: 'Engine', name: 'engine.load', widget: 'value', label: 'Load' },
        { peer: 'Engine', name: 'engine.temp', widget: 'value' },
      ],
    },
  ],
};

describe('DashboardView', () => {
  let fake: FakeWebSockets;

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal('IntersectionObserver', FakeObserver);
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        provideVisibleNow(),
      ],
    });
  });

  afterEach(() => {
    TestBed.resetTestingModule();
    vi.unstubAllGlobals();
    vi.useRealTimers();
    sessionStorage.clear();
  });

  async function settle(fixture: ComponentFixture<unknown>) {
    await vi.advanceTimersByTimeAsync(BATCH_MS);
    fixture.detectChanges();
  }

  const all = (fixture: ComponentFixture<unknown>, css: string) => [
    ...(fixture.nativeElement as HTMLElement).querySelectorAll<HTMLElement>(css),
  ];

  it('creates a widget only when it comes near the screen, and shows its name until then', async () => {
    TestBed.inject(ServerConnection);
    fake.sockets[0].open();
    fake.sockets[0].send(hello);
    const fixture = TestBed.createComponent(DashboardView);
    fixture.componentRef.setInput('spec', SPEC);
    await settle(fixture);

    // 画面の外の部品は作らない: 名前だけの枠が、部品の代わりに並ぶ
    expect(all(fixture, 'app-variable-frame')).toEqual([]);
    const stands = all(fixture, '[data-testid="widget-placeholder"]');
    expect(stands.map((stand) => stand.textContent?.trim())).toEqual([
      'engine.speed',
      'Load',
      'engine.temp',
    ]);

    // 2 つ目が画面に近づいた: その部品だけを作る
    const observer = FakeObserver.last as FakeObserver;
    expect(observer.observed.has(stands[1])).toBe(true);
    observer.tell(stands[1], true);
    // @defer の中でだけ使う NgComponentOutlet は、動的な import で読まれる（偽のタイマーでは進まない）ので、描かれるまで待つ
    await vi.waitFor(
      async () => {
        await settle(fixture);
        expect(all(fixture, '[data-variable]').map((frame) => frame.dataset['variable'])).toEqual([
          'Engine/engine.load',
        ]);
      },
      { timeout: 10_000 },
    );
    expect(
      all(fixture, '[data-testid="widget-placeholder"]').map((stand) => stand.textContent?.trim()),
    ).toEqual(['engine.speed', 'engine.temp']);
  });
});
