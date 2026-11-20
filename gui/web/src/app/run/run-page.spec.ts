import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { pageIntent } from '@vehicle-sil/gui-client/elements';
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

  /** シナリオの一覧の要求（ファイルを選ぶたび、実行が終わるたび）に、例の一覧で答える。 */
  function answerScenarios(): void {
    TestBed.tick();
    for (const request of http.match((r) => r.url.startsWith('api/scenarios'))) {
      request.flush(response('scenarios'));
    }
  }

  /** 照合を頼むまでの間を待って、照合の要求を 1 つ受け取る。 */
  async function checkRequest() {
    answerScenarios();
    await sleep(CHECK_DELAY_MS + 20);
    answerScenarios();
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
    expect(first.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: [],
      manual: [],
      speed: 1,
    });
    expect(first.request.headers.get('Authorization')).toBe('Bearer secret');
    first.flush(response('check-passed'));
    await fixture.whenStable();

    expect(text(fixture, 'check-outcome')).toContain('通った');
    expect(disabled(button(fixture, 'start'))).toBe(false);

    toggle(fixture, 'Dashboard');
    const again = await checkRequest();
    expect(again.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Dashboard'],
      manual: [],
      speed: 1,
    });
    again.flush(response('check-disabled'));
    await fixture.whenStable();
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');

    // 自分で起動する
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLButtonElement>('[data-testid="manual-Engine"] button[role="switch"]')
      ?.click();
    TestBed.tick();
    const manual = await checkRequest();
    expect(manual.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Dashboard'],
      manual: ['Engine'],
      speed: 1,
    });
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
      manual: [],
      speed: 1,
    });
    // 応答の要約をその場で当てる（通知を待たずに、停止が押せて、開始は押せない）
    run.flush(response('run'));
    await fixture.whenStable();
    expect(disabled(button(fixture, 'stop'))).toBe(false);
    expect(disabled(button(fixture, 'start'))).toBe(true);
  });

  it('checks and starts with the same optional stop time, and rejects invalid input', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    input.value = '0';
    input.dispatchEvent(new Event('input'));
    input.dispatchEvent(new Event('blur'));
    TestBed.tick();
    expect(disabled(button(fixture, 'start'))).toBe(true);
    expect(input.getAttribute('aria-invalid')).toBe('true');
    expect(text(fixture, 'blocked')).toContain('停止時刻');
    input.value = '1003';
    input.dispatchEvent(new Event('input'));
    TestBed.tick();
    const checked = await checkRequest();
    expect(checked.request.body).toMatchObject({ speed: 1, stop_at_ms: 1003 });
    expect(pageIntent.get().stop_at_ms).toBe(1003);
    checked.flush(response('check-passed'));
    await fixture.whenStable();
    await sleep(10);
    button(fixture, 'start').click();
    expect(http.expectOne('api/run').request.body).toMatchObject({ speed: 1, stop_at_ms: 1003 });
  });

  it('shows under the status bar why the server refused the start (the problem of the front door)', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    await sleep(10);
    await fixture.whenStable();
    expect(text(fixture, 'action-error')).toBe('');
    button(fixture, 'start').click();
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'shutting-down', message: 'the server is shutting down', detail: null } },
        { status: 503, statusText: 'Service Unavailable' },
      );
    await sleep(0);
    await fixture.whenStable();
    expect(text(fixture, 'action-error')).toContain('操作を受け付けられませんでした');
    const english = (fixture.nativeElement as HTMLElement).querySelector(
      '[data-testid="action-error"] [lang="en"]',
    );
    expect(english?.textContent).toBe('the server is shutting down');
  });

  it('starts paused when asked to, and shows the pause and resume buttons', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    // 実行がない間は、一時停止と再開のボタンを出さない（帯を高くしない）
    const element = fixture.nativeElement as HTMLElement;
    expect(element.querySelector('[data-testid="pause"]')).toBeNull();
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLInputElement>('[data-testid="start-paused"] input')
      ?.click();
    TestBed.tick();
    // 止めて始めるも照合の意図にある: 照合し直してから始める
    const paused = await checkRequest();
    expect(paused.request.body).toMatchObject({ start_paused: true });
    paused.flush(response('check-passed'));
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
    expect(once.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Dashboard', 'Engine'],
      manual: [],
      speed: 1,
    });
    // 応答の前に切り替える: 前の照合の応答が来るまで、次は送らない
    toggle(fixture, 'Engine');
    await sleep(CHECK_DELAY_MS + 20);
    http.expectNone('api/check');
    once.flush(response('check-passed'));
    const next = await checkRequest();
    expect(next.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Dashboard'],
      manual: [],
      speed: 1,
    });
    next.flush(response('check-disabled'));
    await fixture.whenStable();
    // 古い応答（Engine も外したもの）は使わず、最後の組み合わせの結果を出す
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');
    expect(disabled(button(fixture, 'start'))).toBe(false);
  });

  it('does not start without the scenario it has chosen while the list cannot be read', async () => {
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    // A run of simulation.yaml with a scenario has ended: its scenario is chosen for the next run
    for (const message of server('ws-scenario')) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(text(fixture, 'blocked')).toBe('シナリオの一覧を読んでいます。');
    for (const request of http.match((r) => r.url.startsWith('api/scenarios'))) {
      request.flush('', { status: 500, statusText: 'Internal Server Error' });
    }
    await sleep(CHECK_DELAY_MS + 20);
    TestBed.tick();
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    TestBed.tick();
    expect(text(fixture, 'blocked')).toBe(
      'シナリオの一覧を読めないので、選んだシナリオで始められません。',
    );
    expect(disabled(button(fixture, 'start'))).toBe(true);
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
    answerScenarios();
    await fixture.whenStable();

    expect(text(fixture, 'result')).toContain('失敗');
    expect(text(fixture, 'result-card')).toContain('participant-failed');
    expect(text(fixture, 'status-Dashboard')).toContain('終了コード 3');
    expect(button(fixture, 'start').textContent).toContain('もう一度実行');
    expect(text(fixture, 'outcome')).toContain('失敗');
    expect(text(fixture, 'first-error')).toContain('participant-failed');
  });

  it('checks and starts with the networks the parts chose to connect outside, and says so by the start button', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    // 照合の can のネットワークに選択がある。最初は「つながない」
    const part = (fixture.nativeElement as HTMLElement).querySelector(
      '[data-testid="connect-J1939_CAN"] vsil-connect',
    );
    expect(part?.getAttribute('data-chosen')).toBe('none');
    expect(text(fixture, 'connect-summary')).toBe('');

    // 部品が書くのと同じ形（別のネットワークなので、部品は自分では照合しない）
    pageIntent.update({ connect: { ETH_EXT: 'fake0' } });
    try {
      const again = await checkRequest();
      expect(again.request.body).toEqual({
        file: 'bad-lines.yaml',
        disable: [],
        manual: [],
        speed: 1,
        connect: { ETH_EXT: 'fake0' },
      });
      again.flush(response('check-passed'));
      await fixture.whenStable();
      expect(text(fixture, 'connect-summary')).toContain('ETH_EXT → fake0');
      await sleep(10);
      await fixture.whenStable();
      button(fixture, 'start').click();
      expect(http.expectOne('api/run').request.body).toEqual({
        file: 'bad-lines.yaml',
        disable: [],
        manual: [],
        speed: 1,
        connect: { ETH_EXT: 'fake0' },
      });
    } finally {
      pageIntent.update({ connect: undefined });
    }
  });

  it('writes its choices into the page intent, so that a part checks the same intent in one check', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    expect(pageIntent.get()).toEqual({ disable: [], manual: [], speed: 1 });
    // 部品のネットワークに先を選んだ: 部品（約 200 ms 後）と画面（CHECK_DELAY_MS 後）が同じ意図を照合する
    pageIntent.update({ connect: { J1939_CAN: 'can0' } });
    TestBed.tick();
    toggle(fixture, 'Dashboard');
    expect(pageIntent.get()).toEqual({
      connect: { J1939_CAN: 'can0' },
      disable: ['Dashboard'],
      manual: [],
      speed: 1,
    });
    const part = (fixture.nativeElement as HTMLElement).querySelector(
      '[data-testid="connect-J1939_CAN"] vsil-connect',
    );
    expect(part?.getAttribute('data-chosen')).toBe('can0');
    await sleep(CHECK_DELAY_MS + 20);
    answerScenarios();
    const once = http.expectOne('api/check');
    expect(once.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Dashboard'],
      manual: [],
      speed: 1,
      connect: { J1939_CAN: 'can0' },
    });
    once.flush(response('check-disabled'));
    await fixture.whenStable();
    expect(text(fixture, 'check-participant-Dashboard')).toContain('外した');
    http.expectNone('api/check');

    // ページを離れたら、画面が書いた欄は消える（外につなぐ先は部品のもの: 部品が外れるときに消す）
    fixture.destroy();
    try {
      expect(Object.keys(pageIntent.get())).toEqual(['connect']);
    } finally {
      pageIntent.update({ connect: undefined });
    }
  });

  it('writes the choices of another file when the file changes, and writes nothing while a run goes on', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    toggle(fixture, 'Dashboard');
    expect(pageIntent.get()).toMatchObject({ disable: ['Dashboard'] });
    const page = fixture.componentInstance as unknown as {
      selectFile(file: string): void;
      running(): boolean;
    };
    // 別のファイル: 外す participant はそのファイルの選択（ここでは空）に戻り、それを書く
    page.selectFile('forever.yaml');
    TestBed.tick();
    expect(pageIntent.get()).toMatchObject({ disable: [] });
    page.selectFile('bad-lines.yaml');
    TestBed.tick();
    for (const request of http.match('api/check')) request.flush(response('check-passed'));

    // 走っている間は書かない（画面の外で書かれた値もそのまま）。終わったら、いまの選択を書く
    pageIntent.update({ disable: ['Other'] });
    const messages = server('ws-participant-crash');
    // 実行が始まったところまで（要約と最初の事象）
    for (const message of messages.slice(2, 6)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(page.running()).toBe(true);
    expect(pageIntent.get()).toMatchObject({ disable: ['Other'] });
    for (const message of messages.slice(6)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(page.running()).toBe(false);
    expect(pageIntent.get()).toMatchObject({ disable: [] });
    await sleep(CHECK_DELAY_MS + 20);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
  });

  it('does not carry a stop time to another configuration and restores the session value', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    input.value = '123';
    input.dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(pageIntent.get().stop_at_ms).toBe(123);
    const page = fixture.componentInstance as unknown as { selectFile(file: string): void };
    page.selectFile('forever.yaml');
    TestBed.tick();
    expect(input.value).toBe('');
    expect(pageIntent.get().stop_at_ms).toBeUndefined();
    input.value = '321';
    input.dispatchEvent(new Event('input'));
    TestBed.tick();

    // A summary and its check arrive from another session; its stop belongs only to that file.
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.value).toBe('321');
    const otherSession = structuredClone(server('ws-pass')[49]) as { session: { id: number } };
    otherSession.session.id = 2;
    fake.last().send(otherSession);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.value).toBe('321');
    expect(pageIntent.get().stop_at_ms).toBe(321);
    page.selectFile('simulation.yaml');
    TestBed.tick();
    expect(input.value).toBe('1000');
    expect(pageIntent.get().stop_at_ms).toBe(1000);
  });

  it('disables the stop time until the first hello and replay have synchronized', async () => {
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    TestBed.tick();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(input.disabled).toBe(true);
    const messages = server('ws-replay');
    fake.last().send(messages[0]);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.disabled).toBe(true);
    for (const message of messages.slice(1)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.disabled).toBe(false);
    expect(input.value).toBe('1000');
  });

  it('keeps an edited stop time through a reconnect to the same session', async () => {
    const fixture = await openWithFiles();
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(input.value).toBe('1000');
    input.value = '123';
    input.dispatchEvent(new Event('input'));
    TestBed.tick();
    const before = fake.last();
    before.fail();
    TestBed.tick();
    expect(input.disabled).toBe(true);
    await sleep(600);
    const after = fake.last();
    expect(after).not.toBe(before);
    after.open();
    for (const message of server('ws-replay')) after.send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.disabled).toBe(false);
    expect(input.value).toBe('123');
    expect(pageIntent.get().stop_at_ms).toBe(123);
  });

  for (const mode of ['untouched', 'carried-edit', 'edit-now'] as const) {
    it(`restores a delayed session check after a language change (${mode})`, async () => {
      sessionStorage.setItem(
        'vehicle-sil-gui-carry',
        JSON.stringify({
          run: {
            session: 1,
            chosen: 'simulation.yaml',
            file: 'simulation.yaml',
            disable: [],
            manual: [],
            speed: 1,
            stopAt: '',
            stopAtEdited: mode === 'carried-edit',
            logSource: '',
          },
        }),
      );
      const fixture = await openWithFiles();
      const summary = structuredClone(server('ws-pass')[3]) as {
        session: { state: string; ended_at: string; launcher: { running: boolean } };
      };
      summary.session.state = 'ended';
      summary.session.ended_at = '2026-01-01T00:00:01Z';
      summary.session.launcher.running = false;
      fake.last().send(summary);
      await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
      TestBed.tick();
      const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
        '[data-testid="stop-at-ms"]',
      )!;
      expect(input.value).toBe('');
      if (mode === 'edit-now') {
        input.value = '250';
        input.dispatchEvent(new Event('input'));
        TestBed.tick();
      }
      fake.last().send(server('ws-pass')[8]);
      await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
      TestBed.tick();
      expect(input.value).toBe(mode === 'untouched' ? '1000' : mode === 'edit-now' ? '250' : '');
    });
  }

  it('clears the prior stop time when another session starts for the same configuration', async () => {
    const fixture = await openWithFiles();
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(input.value).toBe('1000');
    const summary = structuredClone(server('ws-pass')[3]) as { session: { id: number } };
    summary.session.id = 2;
    fake.last().send(summary);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.value).toBe('');
    const check = structuredClone(server('ws-pass')[8]) as {
      session: number;
      event: { check: { stop_at_ms: number } };
    };
    check.session = 2;
    check.event.check.stop_at_ms = 2000;
    fake.last().send(check);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    expect(input.value).toBe('2000');
  });

  it('checks the speed and the paused start with the rest, and does not start on what the check refuses', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    const element = fixture.nativeElement as HTMLElement;
    pageIntent.update({ connect: { ETH_EXT: 'fake0' } });
    try {
      [...element.querySelectorAll<HTMLButtonElement>('app-speed-picker mat-button-toggle button')]
        .find((b) => b.textContent?.includes('倍率'))
        ?.click();
      TestBed.tick();
      const factor = element.querySelector<HTMLInputElement>('[data-testid="speed-factor"]')!;
      // 打っている途中: 読めない値は意図に入らない（照合は最後に読めた速さのまま、1 回だけ）
      for (const text of ['2', '2x']) {
        factor.value = text;
        factor.dispatchEvent(new Event('input'));
        TestBed.tick();
      }
      element.querySelector<HTMLInputElement>('[data-testid="start-paused"] input')?.click();
      TestBed.tick();
      const checked = await checkRequest();
      expect(checked.request.body).toEqual({
        file: 'bad-lines.yaml',
        disable: [],
        manual: [],
        speed: 2,
        start_paused: true,
        connect: { ETH_EXT: 'fake0' },
      });
      expect(pageIntent.get()).toMatchObject({ speed: 2, start_paused: true });
      // 実時間を保つ実行は、その速さと止めて始めるを照合で断る: 始める前に所見が出て、開始は止まる
      const refused = structuredClone(response('check-failed')) as {
        check: { findings: { code: string; message: string }[] };
      };
      refused.check.findings[0]!.code = 'outside-real-time';
      refused.check.findings[0]!.message = 'the run keeps real time: leave the speed out.';
      checked.flush(refused);
      await fixture.whenStable();
      expect(text(fixture, 'check-card')).toContain('outside-real-time');
      expect(disabled(button(fixture, 'start'))).toBe(true);
      // 倍率を直すまでは、速度の誤りが開始を止める
      expect(text(fixture, 'blocked')).toContain('速度の倍率が範囲の外です。');
    } finally {
      pageIntent.update({ connect: undefined });
    }
  });

  it('does not send a participant both left out and started by hand', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLButtonElement>('[data-testid="manual-Engine"] button[role="switch"]')
      ?.click();
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await fixture.whenStable();
    // 外すと「自分で起動する」も外れる（check-panel）。ランチャーは両方に入った名前を照合で断る
    toggle(fixture, 'Engine');
    const again = await checkRequest();
    expect(again.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: ['Engine'],
      manual: [],
      speed: 1,
    });
  });

  it('shows in the result why a run connected outside failed, in the launcher words', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    const lost = {
      code: 'connect-lost',
      message: 'the bridge of ETH_EXT ended while the simulation was running',
    };
    // 参加者の失敗を、外につないだブリッジの失敗に置き換えた実行（事象と要約の結果の両方）
    const failed = 'participant-failed';
    const messages = server('ws-participant-crash')
      .slice(2)
      .map(
        (message) =>
          JSON.parse(
            JSON.stringify(message).replace(
              /"code":"participant-failed"(.*?)"message":"[^"]*"/g,
              `"code":"${lost.code}"$1"message":"${lost.message}"`,
            ),
          ) as unknown,
      );
    expect(JSON.stringify(messages)).not.toContain(failed);
    for (const message of messages) fake.last().send(message);
    await new Promise((resolve) => setTimeout(resolve, BATCH_MS + 20));
    TestBed.tick();
    await sleep(CHECK_DELAY_MS + 20);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
    await fixture.whenStable();
    expect(text(fixture, 'result-card')).toContain('connect-lost');
    expect(text(fixture, 'result-card')).toContain(lost.message);
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
    expect(readRunChoices({ ...choices, manual: ['A', 1] })).toBeNull();
    expect(readRunChoices({ ...choices, stopAtEdited: true })).toBeNull();
    expect(readRunChoices({ ...choices, stopAtEdited: 'yes' })).toBeNull();
  });
});
