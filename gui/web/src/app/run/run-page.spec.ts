import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { FakeWebSockets } from '../core/streams/fake-web-socket';
import { protocolExamples } from '../testing/protocol-examples';
import { RunPage } from './run-page';
import { BATCH_MS } from './run-session';

describe('RunPage', () => {
  let examples: Map<string, unknown>;
  let fake: FakeWebSockets;
  let http: HttpTestingController;

  beforeAll(async () => {
    examples = await protocolExamples();
  });

  function response(name: string): object {
    return (examples.get(name) as { response: object }).response;
  }

  function server(name: string): unknown[] {
    return (examples.get(name) as { server: unknown[] }).server;
  }

  beforeEach(async () => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [RunPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        fake.provider,
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => sessionStorage.clear());

  function text(fixture: ComponentFixture<RunPage>, testId: string): string {
    const element = fixture.nativeElement as HTMLElement;
    return (element.querySelector(`[data-testid="${testId}"]`)?.textContent ?? '')
      .replace(/\s+/g, ' ')
      .trim();
  }

  function button(fixture: ComponentFixture<RunPage>, testId: string): HTMLButtonElement {
    const found = (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="${testId}"]`);
    if (!(found instanceof HTMLButtonElement)) throw new Error(`no button ${testId}`);
    return found;
  }

  /** 画面を開き、サーバーの情報と一覧に答え、最初の照合の要求まで進める。 */
  async function openWithFiles(): Promise<ComponentFixture<RunPage>> {
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    return fixture;
  }

  it('asks to be opened from the server URL when there is no token', async () => {
    sessionStorage.clear();
    const fixture = TestBed.createComponent(RunPage);
    await fixture.whenStable();
    expect(text(fixture, 'no-token')).toContain('#token=');
    http.expectNone('api/app');
    expect(fake.sockets).toHaveLength(0);
  });

  it('checks the chosen file, and checks again without a participant that is switched off', async () => {
    const fixture = await openWithFiles();
    // 起動のときのファイルがないので、一覧の最初のファイル
    const first = http.expectOne('api/check');
    expect(first.request.body).toEqual({ file: 'bad-lines.yaml', disable: [] });
    expect(first.request.headers.get('Authorization')).toBe('Bearer secret');
    first.flush(response('check-passed'));
    await fixture.whenStable();

    expect(text(fixture, 'check-outcome')).toContain('通った');
    expect(button(fixture, 'start').disabled).toBe(false);

    const toggle = (fixture.nativeElement as HTMLElement).querySelector<HTMLButtonElement>(
      '[data-testid="check-participant-Dashboard"] button[role="switch"]',
    );
    toggle?.click();
    TestBed.tick();
    const again = http.expectOne('api/check');
    expect(again.request.body).toEqual({ file: 'bad-lines.yaml', disable: ['Dashboard'] });
    again.flush(response('check-disabled'));
    await fixture.whenStable();
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');
  });

  it('does not start while the check finds errors, and says why', async () => {
    const fixture = await openWithFiles();
    http.expectOne('api/check').flush(response('check-failed'));
    await fixture.whenStable();
    expect(button(fixture, 'start').disabled).toBe(true);
    expect(text(fixture, 'blocked')).toContain('誤り 1 件');
    expect(text(fixture, 'check-card')).toContain('step-mismatch');
  });

  it('starts with the chosen settings', async () => {
    const fixture = await openWithFiles();
    http.expectOne('api/check').flush(response('check-passed'));
    await fixture.whenStable();
    button(fixture, 'start').click();
    const run = http.expectOne('api/run');
    expect(run.request.method).toBe('POST');
    expect(run.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: [],
      speed: null,
      start_paused: false,
    });
  });

  it('shows the result and the reason of a failed run, and offers to run again', async () => {
    const fixture = await openWithFiles();
    http.expectOne('api/check').flush(response('check-passed'));
    for (const message of server('ws-participant-crash').slice(2)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    // 終わったので、選んだファイル（セッションのもの）を照合し直す
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    await fixture.whenStable();

    expect(text(fixture, 'result')).toContain('失敗');
    expect(text(fixture, 'result-card')).toContain('participant-failed');
    expect(text(fixture, 'status-Dashboard')).toContain('終了コード 3');
    expect(button(fixture, 'start').textContent).toContain('もう一度実行');
  });
});
