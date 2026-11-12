import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { FakeWebSockets, protocolExamples } from '@vehicle-sil/gui-client/testing';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { CHECK_DELAY_MS, RunPage, readRunChoices } from './run-page';
import { ACTION_GUARD } from './run-status';

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
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        // 押せるようになったばかりのボタンを押しても何もしない間（試験ではすぐ）
        { provide: ACTION_GUARD, useValue: 0 },
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

  /** 押せないか（開始と停止は、フォーカスを残すため aria-disabled で押せなくする）。 */
  const disabled = (b: HTMLButtonElement) =>
    b.disabled || b.getAttribute('aria-disabled') === 'true';

  const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

  /** 照合を頼むまでの間を待って、照合の要求を 1 つ受け取る。 */
  async function checkRequest() {
    await sleep(CHECK_DELAY_MS + 20);
    return http.expectOne('api/check');
  }

  function toggle(fixture: ComponentFixture<RunPage>, name: string): void {
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLButtonElement>(
        `[data-testid="check-participant-${name}"] button[role="switch"]`,
      )
      ?.click();
    TestBed.tick();
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
    const first = await checkRequest();
    expect(first.request.body).toEqual({ file: 'bad-lines.yaml', disable: [] });
    expect(first.request.headers.get('Authorization')).toBe('Bearer secret');
    first.flush(response('check-passed'));
    await fixture.whenStable();

    expect(text(fixture, 'check-outcome')).toContain('通った');
    expect(disabled(button(fixture, 'start'))).toBe(false);

    toggle(fixture, 'Dashboard');
    const again = await checkRequest();
    expect(again.request.body).toEqual({ file: 'bad-lines.yaml', disable: ['Dashboard'] });
    again.flush(response('check-disabled'));
    await fixture.whenStable();
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');
  });

  it('does not start while the check finds errors, and says why', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-failed'));
    await fixture.whenStable();
    expect(disabled(button(fixture, 'start'))).toBe(true);
    expect(text(fixture, 'blocked')).toContain('誤り 1 件');
    expect(text(fixture, 'check-card')).toContain('step-mismatch');
  });

  it('starts with the chosen settings', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    expect(button(fixture, 'start').textContent).toContain('開始');
    await sleep(10);
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
    // 応答の要約をその場で当てる（通知を待たずに、停止が押せて、開始は押せない）
    run.flush(response('run'));
    await fixture.whenStable();
    expect(disabled(button(fixture, 'stop'))).toBe(false);
    expect(disabled(button(fixture, 'start'))).toBe(true);
  });

  it('starts paused when asked to, and shows the pause and resume buttons of a launcher that has them', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    // 実行がない間は、一時停止と再開のボタンを出さない（帯を高くしない）
    const element = fixture.nativeElement as HTMLElement;
    expect(element.querySelector('[data-testid="pause"]')).toBeNull();
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLInputElement>('[data-testid="start-paused"] input')
      ?.click();
    await fixture.whenStable();
    await sleep(10);
    await fixture.whenStable();
    button(fixture, 'start').click();
    expect(http.expectOne('api/run').request.body).toMatchObject({ start_paused: true });
    // 開始を頼んだら、停止の後に一時停止と再開（動き出すまでは押せない）
    await fixture.whenStable();
    const actions = [...element.querySelectorAll('app-run-status .actions button')].map((b) =>
      b.getAttribute('data-testid'),
    );
    expect(actions).toEqual(['start', 'stop', 'pause', 'resume']);
    expect(disabled(button(fixture, 'pause'))).toBe(true);
    expect(disabled(button(fixture, 'resume'))).toBe(true);
  });

  it('checks once after quick switches, after the answer to the check before', async () => {
    const fixture = await openWithFiles();
    const first = await checkRequest();
    first.flush(response('check-passed'));
    await fixture.whenStable();
    // 続けて 2 回切り替える: 最後の組み合わせで 1 回だけ
    toggle(fixture, 'Dashboard');
    toggle(fixture, 'Engine');
    const once = await checkRequest();
    expect(once.request.body).toEqual({ file: 'bad-lines.yaml', disable: ['Dashboard', 'Engine'] });
    // 応答の前に切り替える: 前の照合の応答が来るまで、次は送らない
    toggle(fixture, 'Engine');
    await sleep(CHECK_DELAY_MS + 20);
    http.expectNone('api/check');
    once.flush(response('check-passed'));
    const next = await checkRequest();
    expect(next.request.body).toEqual({ file: 'bad-lines.yaml', disable: ['Dashboard'] });
    next.flush(response('check-disabled'));
    await fixture.whenStable();
    // 古い応答（Engine も外したもの）は使わず、最後の組み合わせの結果を出す
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');
    expect(disabled(button(fixture, 'start'))).toBe(false);
  });

  it('shows the result and the reason of a failed run, and offers to run again', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    for (const message of server('ws-participant-crash').slice(2)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    // 終わったので、選んだファイル（セッションのもの）を照合し直す
    await sleep(CHECK_DELAY_MS + 20);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    await fixture.whenStable();

    expect(text(fixture, 'result')).toContain('失敗');
    expect(text(fixture, 'result-card')).toContain('participant-failed');
    expect(text(fixture, 'status-Dashboard')).toContain('終了コード 3');
    expect(button(fixture, 'start').textContent).toContain('もう一度実行');
    expect(text(fixture, 'outcome')).toContain('失敗');
    expect(text(fixture, 'first-error')).toContain('participant-failed');
  });
});

describe('readRunChoices', () => {
  const choices = {
    session: 3,
    chosen: 'a.yaml',
    file: 'a.yaml',
    disable: ['Logger'],
    speed: 'max',
    logSource: '',
  };

  it('reads the choices carried over a change of language', () => {
    expect(readRunChoices(choices)).toEqual(choices);
    expect(readRunChoices({ ...choices, session: null, chosen: null, speed: 2.5 })).not.toBeNull();
    expect(readRunChoices({ ...choices, startPaused: true })?.startPaused).toBe(true);
  });

  it('carries nothing when the shape is wrong', () => {
    expect(readRunChoices(undefined)).toBeNull();
    expect(readRunChoices({ ...choices, disable: [1] })).toBeNull();
    expect(readRunChoices({ ...choices, speed: 'fast' })).toBeNull();
    expect(readRunChoices({ ...choices, logSource: undefined })).toBeNull();
    expect(readRunChoices({ ...choices, startPaused: 'yes' })).toBeNull();
  });
});
