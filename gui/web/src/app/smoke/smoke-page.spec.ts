// 通しの確認用（後で捨てる）。httpResource と偽の WebSocket を使う試験の見本。
import { provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { FakeWebSockets } from '../core/streams/fake-web-socket';
import { TelemetrySample } from '../core/api';
import { SmokePage } from './smoke-page';

describe('SmokePage', () => {
  let http: HttpTestingController;
  let fake: FakeWebSockets;

  beforeEach(async () => {
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [SmokePage],
      providers: [provideHttpClient(), provideHttpClientTesting(), fake.provider],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => http.verify());

  function text(element: HTMLElement, testId: string): string | undefined {
    return element
      .querySelector(`[data-testid="${testId}"]`)
      ?.textContent?.replace(/\s+/g, ' ')
      .trim();
  }

  // 応答を返していない要求があるあいだは whenStable() が終わらない。先に TestBed.tick() で描画して要求を出させる。
  function render() {
    const fixture = TestBed.createComponent(SmokePage);
    TestBed.tick();
    return fixture;
  }

  it('shows the health returned by the server', async () => {
    const fixture = render();
    expect(text(fixture.nativeElement, 'health')).toContain('問い合わせ中');

    http.expectOne('api/health').flush({ status: 'ok', server: 'stub' });
    await fixture.whenStable();

    expect(text(fixture.nativeElement, 'health')).toBe('状態: ok（stub）');
  });

  it('shows a failed health request', async () => {
    const fixture = render();

    http.expectOne('api/health').flush('down', { status: 503, statusText: 'Service Unavailable' });
    await fixture.whenStable();

    expect(text(fixture.nativeElement, 'health')).toContain('取得できない');
  });

  it('shows the WebSocket values and stops listening when it goes away', async () => {
    const fixture = render();
    http.expectOne('api/health').flush({ status: 'ok', server: 'stub' });
    await fixture.whenStable();
    expect(text(fixture.nativeElement, 'ws-state')).toBe('connecting');

    const socket = fake.last<TelemetrySample>();
    socket.open();
    socket.send({ seq: 3, values: { speed: 1234.5678 } });
    await fixture.whenStable();

    expect(text(fixture.nativeElement, 'ws-state')).toBe('open');
    expect(text(fixture.nativeElement, 'ws-seq')).toBe('3');
    expect(text(fixture.nativeElement, 'value-speed')).toBe('1234.57');

    fixture.destroy();
    await new Promise((resolve) => setTimeout(resolve, 1100));
    expect(socket.closedByClient).toBe(true);
  });
});
