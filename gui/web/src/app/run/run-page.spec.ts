import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { pageIntent } from '@vehicle-sil/gui-client/elements';
import { FakeWebSockets, protocolExamples } from '@vehicle-sil/gui-client/testing';
import { LanguageChoice, PAGE_RELOAD } from '../core/i18n/language-choice';
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
    const result = (examples.get(name) as { response: object }).response;
    return name === 'run' ? { ...result, instance: '0123456789abcdef' } : result;
  }

  function server(name: string): unknown[] {
    return (examples.get(name) as { server: unknown[] }).server;
  }

  beforeEach(async () => {
    vi.useFakeTimers();
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

  afterEach(() => {
    TestBed.resetTestingModule();
    vi.useRealTimers();
    sessionStorage.clear();
  });

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

  const advance = (ms: number) => vi.advanceTimersByTimeAsync(ms);
  const settle = async () => {
    await advance(0);
    TestBed.tick();
  };

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
    await advance(CHECK_DELAY_MS);
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
    await advance(BATCH_MS);
    TestBed.tick();
    return fixture;
  }

  it('does not run a check until the server protocol is accepted', async () => {
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush({ ...response('app'), vehicle_sil_gui: 2 });
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send({ ...(server('ws-pass')[0] as object), vehicle_sil_gui: 2 });
    await advance(CHECK_DELAY_MS + BATCH_MS);
    TestBed.tick();
    expect(text(fixture, 'incompatible')).toContain('server speaks version 2');
    http.expectNone('api/check');
    http.expectNone('api/run');
  });

  it('asks to be opened from the server URL when there is no token and the server rejects it', async () => {
    sessionStorage.clear();
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    const appReq = http.expectOne('api/app');
    expect(appReq.request.headers.has('Authorization')).toBe(false);
    appReq.flush(
      { error: { code: 'unauthorized', message: 'missing token', detail: null } },
      { status: 401, statusText: 'Unauthorized' },
    );
    const configsReq = http.expectOne('api/configs');
    configsReq.flush(
      { error: { code: 'unauthorized', message: 'missing token', detail: null } },
      { status: 401, statusText: 'Unauthorized' },
    );
    await settle();
    expect(text(fixture, 'no-token')).toContain('#token=');
  });

  it('works without a token when the server allows --no-auth', async () => {
    sessionStorage.clear();
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    const appReq = http.expectOne('api/app');
    expect(appReq.request.headers.has('Authorization')).toBe(false);
    appReq.flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();
    const check = await checkRequest();
    expect(check.request.headers.has('Authorization')).toBe(false);
    check.flush(response('check-passed'));
    await settle();
    expect(fixture.nativeElement.querySelector('[data-testid="no-token"]')).toBeNull();
    expect(text(fixture, 'check-outcome')).toContain('通った');
    expect(disabled(button(fixture, 'start'))).toBe(false);
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
    await settle();

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
    await settle();
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
    await settle();
    expect(disabled(button(fixture, 'start'))).toBe(true);
    expect(text(fixture, 'blocked')).toContain('誤り 1 件');
    expect(text(fixture, 'check-card')).toContain('step-mismatch');
  });

  it('starts with the chosen settings', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
    expect(button(fixture, 'start').textContent).toContain('開始');
    await advance(1);
    await settle();
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
    await settle();
    expect(disabled(button(fixture, 'stop'))).toBe(false);
    expect(disabled(button(fixture, 'start'))).toBe(true);
  });

  it('checks and starts with the same optional stop time, and rejects invalid input', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    const described = () =>
      (input.getAttribute('aria-describedby') ?? '')
        .split(/\s+/)
        .filter(Boolean)
        .map(
          (id) =>
            (fixture.nativeElement as HTMLElement).querySelector(`[id="${id}"]`)?.textContent ?? '',
        );
    expect(described().some((text) => text.includes('空欄なら自動停止しません'))).toBe(true);
    input.value = '0';
    input.dispatchEvent(new Event('input'));
    input.dispatchEvent(new Event('blur'));
    TestBed.tick();
    expect(disabled(button(fixture, 'start'))).toBe(true);
    expect(input.getAttribute('aria-invalid')).toBe('true');
    expect(described().some((text) => text.includes('空欄なら自動停止しません'))).toBe(true);
    expect(described().some((text) => text.includes('停止時刻は 1 から'))).toBe(true);
    expect(text(fixture, 'blocked')).toContain('停止時刻');
    input.value = '1003';
    input.dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(input.getAttribute('aria-invalid')).toBe('false');
    expect(described().some((text) => text.includes('空欄なら自動停止しません'))).toBe(true);
    expect(described().some((text) => text.includes('停止時刻は 1 から'))).toBe(false);
    const checked = await checkRequest();
    expect(checked.request.body).toMatchObject({ speed: 1, stop_at_ms: 1003 });
    expect(pageIntent.get().stop_at_ms).toBe(1003);
    checked.flush(response('check-passed'));
    await settle();
    await advance(1);
    button(fixture, 'start').click();
    expect(http.expectOne('api/run').request.body).toMatchObject({ speed: 1, stop_at_ms: 1003 });
  });

  it('shows under the status bar why the server refused the start (the problem of the front door)', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
    await advance(1);
    await settle();
    expect(text(fixture, 'action-error')).toBe('');
    button(fixture, 'start').click();
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'shutting-down', message: 'the server is shutting down', detail: null } },
        { status: 503, statusText: 'Service Unavailable' },
      );
    await advance(0);
    await settle();
    expect(text(fixture, 'action-error')).toContain('操作を受け付けられませんでした');
    const english = (fixture.nativeElement as HTMLElement).querySelector(
      '[data-testid="action-error"] [lang="en"]',
    );
    expect(english?.textContent).toBe('the server is shutting down');
  });

  it('starts paused when asked to, and shows the pause and resume buttons', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
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
    await settle();
    await advance(1);
    await settle();
    button(fixture, 'start').click();
    expect(http.expectOne('api/run').request.body).toMatchObject({ start_paused: true });
    // 開始を頼んだら、停止の後に一時停止と再開（動き出すまでは押せない）
    await settle();
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
    await settle();
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
    await advance(CHECK_DELAY_MS);
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
    await settle();
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
    await advance(BATCH_MS);
    TestBed.tick();
    expect(text(fixture, 'blocked')).toBe('シナリオの一覧を読んでいます。');
    for (const request of http.match((r) => r.url.startsWith('api/scenarios'))) {
      request.flush('', { status: 500, statusText: 'Internal Server Error' });
    }
    await advance(CHECK_DELAY_MS);
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
    await advance(BATCH_MS);
    TestBed.tick();
    // 終わったので、選んだファイル（セッションのもの）を照合し直す
    await advance(CHECK_DELAY_MS);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
    await settle();

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
    await settle();
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
      await settle();
      expect(text(fixture, 'connect-summary')).toContain('ETH_EXT → fake0');
      await advance(1);
      await settle();
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
    await settle();
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
    await advance(CHECK_DELAY_MS);
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
    await settle();
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
    await settle();
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
    await advance(BATCH_MS);
    TestBed.tick();
    expect(page.running()).toBe(true);
    expect(pageIntent.get()).toMatchObject({ disable: ['Other'] });
    for (const message of messages.slice(6)) fake.last().send(message);
    await advance(BATCH_MS);
    TestBed.tick();
    expect(page.running()).toBe(false);
    expect(pageIntent.get()).toMatchObject({ disable: [] });
    await advance(CHECK_DELAY_MS);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
  });

  it('does not carry a stop time to another configuration and restores the session value', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
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
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.value).toBe('321');
    const otherSession = structuredClone(server('ws-pass')[49]) as {
      stream_seq?: number;
      session: { id: string };
    };
    otherSession.stream_seq = 100;
    otherSession.session.id = '2';
    fake.last().send(otherSession);
    await advance(BATCH_MS);
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
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.disabled).toBe(true);
    for (const message of messages.slice(1)) fake.last().send(message);
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.disabled).toBe(false);
    expect(input.value).toBe('1000');
  });

  it('keeps an edited stop time through a reconnect to the same session', async () => {
    const fixture = await openWithFiles();
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await advance(BATCH_MS);
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
    await advance(600);
    const after = fake.last();
    expect(after).not.toBe(before);
    after.open();
    for (const message of server('ws-replay')) after.send(message);
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.disabled).toBe(false);
    expect(input.value).toBe('123');
    expect(pageIntent.get().stop_at_ms).toBe(123);
  });

  it('restores an edited stop time for a selected file different from the last session', async () => {
    sessionStorage.setItem(
      'vehicle-sil-gui-carry',
      JSON.stringify({
        run: {
          session: '1',
          chosen: 'forever.yaml',
          file: 'forever.yaml',
          disable: [],
          manual: [],
          speed: 1,
          stopAt: '321',
          stopAtEdited: true,
          stopSession: null,
          logSource: '',
        },
      }),
    );
    const fixture = await openWithFiles();
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await advance(BATCH_MS);
    TestBed.tick();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(input.value).toBe('321');
  });

  for (const mode of ['untouched', 'carried-edit', 'edit-now'] as const) {
    it(`restores a delayed session check after a language change (${mode})`, async () => {
      sessionStorage.setItem(
        'vehicle-sil-gui-carry',
        JSON.stringify({
          run: {
            session: '1',
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
      await advance(BATCH_MS);
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
      await advance(BATCH_MS);
      TestBed.tick();
      expect(input.value).toBe(mode === 'untouched' ? '1000' : mode === 'edit-now' ? '250' : '');
    });
  }

  it('clears the prior stop time when another session starts for the same configuration', async () => {
    const fixture = await openWithFiles();
    for (const message of server('ws-pass').slice(2)) fake.last().send(message);
    await advance(BATCH_MS);
    TestBed.tick();
    const input = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(input.value).toBe('1000');
    const summary = structuredClone(server('ws-pass')[3]) as {
      stream_seq?: number;
      session: { id: string };
    };
    summary.stream_seq = 100;
    summary.session.id = '2';
    fake.last().send(summary);
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.value).toBe('');
    const check = structuredClone(server('ws-pass')[8]) as {
      stream_seq?: number;
      run_id: string;
      event: { check: { stop_at_ms: number } };
    };
    check.stream_seq = 101;
    check.run_id = '2';
    check.event.check.stop_at_ms = 2000;
    fake.last().send(check);
    await advance(BATCH_MS);
    TestBed.tick();
    expect(input.value).toBe('2000');
  });

  it('checks the speed and the paused start with the rest, and does not start on what the check refuses', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
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
      await settle();
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
    await settle();
    (fixture.nativeElement as HTMLElement)
      .querySelector<HTMLButtonElement>('[data-testid="manual-Engine"] button[role="switch"]')
      ?.click();
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
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
    await advance(BATCH_MS);
    TestBed.tick();
    await advance(CHECK_DELAY_MS);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
    await settle();
    expect(text(fixture, 'result-card')).toContain('connect-lost');
    expect(text(fixture, 'result-card')).toContain(lost.message);
  });

  it('controls after_scenario handover option, omits stop_at_ms on pause, and disables handover when unsupported', async () => {
    const appWithHandover = () => {
      const base = response('app') as { launcher: { version: { features: string[] } } };
      return {
        ...base,
        launcher: {
          ...base.launcher,
          version: {
            ...base.launcher.version,
            features: [...new Set([...base.launcher.version.features, 'scenario-handover'])],
          },
        },
      };
    };
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithHandover());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();

    // No after-scenario selector when no scenario is chosen
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="after-scenario"]'),
    ).toBeNull();

    // Enter invalid stopAt first ('0') and select a scenario
    const stopInput = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    stopInput.value = '0';
    stopInput.dispatchEvent(new Event('input'));
    stopInput.dispatchEvent(new Event('blur'));
    TestBed.tick();
    expect(disabled(button(fixture, 'start'))).toBe(true);

    const page = fixture.componentInstance as unknown as {
      selectScenario(s: string | null): void;
      selectAfterScenario(v: 'stop' | 'pause'): void;
    };
    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    page.selectScenario(firstScenario);
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();

    // Default is 'stop', which keeps stop_at_ms validation active and omits after_scenario
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="after-scenario"]'),
    ).not.toBeNull();
    expect(disabled(button(fixture, 'start'))).toBe(true);

    // Fix stopAt to '1003', verify it sends stop_at_ms when 'stop' is chosen
    stopInput.value = '1003';
    stopInput.dispatchEvent(new Event('input'));
    TestBed.tick();
    const stopCheck = await checkRequest();
    expect(stopCheck.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: [],
      manual: [],
      scenario: firstScenario,
      speed: 1,
      stop_at_ms: 1003,
    });
    stopCheck.flush(response('check-passed'));
    await settle();

    // Switch to 'pause' (handover): stop-at-ms becomes disabled/cleared, stop_at_ms omitted everywhere
    const pauseRadio = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="after-scenario-pause"] input[type="radio"]',
    )!;
    pauseRadio.click();
    TestBed.tick();
    expect(stopInput.disabled).toBe(true);
    expect(stopInput.value).toBe('');
    expect(pageIntent.get()).toMatchObject({
      scenario: firstScenario,
      after_scenario: 'pause',
    });
    expect(pageIntent.get().stop_at_ms).toBeUndefined();
    const pauseCheck = await checkRequest();
    expect(pauseCheck.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: [],
      manual: [],
      scenario: firstScenario,
      after_scenario: 'pause',
      speed: 1,
    });
    expect((pauseCheck.request.body as Record<string, unknown>)['stop_at_ms']).toBeUndefined();
    pauseCheck.flush(response('check-passed'));
    await advance(1);
    await settle();
    expect(disabled(button(fixture, 'start'))).toBe(false);

    button(fixture, 'start').click();
    const runReq = http.expectOne('api/run');
    expect(runReq.request.body).toEqual({
      file: 'bad-lines.yaml',
      disable: [],
      manual: [],
      scenario: firstScenario,
      after_scenario: 'pause',
      speed: 1,
    });
    runReq.flush(response('run'));

    // Unsupported launcher disables 'pause' radio and shows hint
    TestBed.resetTestingModule();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [RunPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        { provide: ACTION_GUARD, useValue: 0 },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
    const appWithoutHandover = () => {
      const base = response('app') as { launcher: { version: { features: string[] } } };
      return {
        ...base,
        launcher: {
          ...base.launcher,
          version: {
            ...base.launcher.version,
            features: base.launcher.version.features.filter((f) => f !== 'scenario-handover'),
          },
        },
      };
    };
    const unsupportedFixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithoutHandover());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
    const unsupportedPage = unsupportedFixture.componentInstance as unknown as {
      selectScenario(s: string | null): void;
    };
    unsupportedPage.selectScenario(firstScenario);
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();
    const unsupportedPauseRadio = (
      unsupportedFixture.nativeElement as HTMLElement
    ).querySelector<HTMLInputElement>('[data-testid="after-scenario-pause"] input[type="radio"]')!;
    expect(unsupportedPauseRadio.disabled).toBe(true);
    expect(text(unsupportedFixture, 'handover-unsupported')).toContain('対応していません');
  });

  it('keeps time controls and shows handover badge when handed over, and hides it on non-handover outcomes', async () => {
    const fixture = await openWithFiles();
    (await checkRequest()).flush(response('check-passed'));
    await settle();

    const ok = { ok: true, reason: null, message: null, caution: null };
    const scenarioSummary = {
      file: 'scenarios/a.yaml',
      name: 'a',
      step_count: 1,
      steps: [],
      steps_omitted: 0,
      status: null,
      over_at_ms: null,
      timeline_url: null,
    };
    const baseSession = (
      (server('ws-pass')[3] ?? (server('ws-pass')[2] as unknown)) as {
        session: Record<string, unknown>;
      }
    ).session;
    const runningHandoverSummary = {
      ...baseSession,
      id: '9',
      file: 'bad-lines.yaml',
      state: 'running',
      phase: 'running',
      result: null,
      launcher: { running: true, exit_code: null, signal: null, silent_s: null },
      scenario: scenarioSummary,
      launcher_state: {
        seq: 5,
        rev: 5,
        phase: 'running',
        paused: true,
        paused_at_ms: 500,
        holding: false,
        speed: 'max',
        pacing: 'not-paced',
        stop: { requests: 0, by: null },
        scenario: true,
        handed_over: true,
        scenario_over_at_ms: 500,
        allowed: {
          pause: { ok: false, reason: 'paused', message: 'already paused', caution: null },
          resume: ok,
          step: { ...ok, min_ms: 10, max_ms: 5000 },
          speed: { ...ok, min: 0.1, max: 100 },
          run_until: ok,
          stop: ok,
          write: ok,
        },
        outside: [],
      },
    };
    fake.last().send({ type: 'session', stream_seq: 10, session: runningHandoverSummary });
    fake.last().send({ type: 'synced', stream_seq: 11, run_id: '9', last_seq: 5 });
    await advance(BATCH_MS);
    TestBed.tick();
    expect(text(fixture, 'scenario-handover')).toContain('500 ms');
    expect(text(fixture, 'outcome')).toContain('一時停止中');
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="time-card"]'),
    ).not.toBeNull();
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="result-card"]'),
    ).toBeNull();

    // Crashed session never shows handover badge
    fake.last().send({
      type: 'session',
      stream_seq: 12,
      session: {
        ...runningHandoverSummary,
        state: 'ended',
        launcher: { running: false, exit_code: 1, signal: null, silent_s: null },
      },
    });
    await advance(BATCH_MS);
    TestBed.tick();
    await advance(CHECK_DELAY_MS);
    for (const request of http.match('api/check')) request.flush(response('check-passed'));
    answerScenarios();
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="scenario-handover"]'),
    ).toBeNull();
  });

  it('preserves carried afterScenario pause and keeps stopAt disabled/omitted across language switch when selected file differs from session file', async () => {
    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    sessionStorage.setItem(
      'vehicle-sil-gui-carry',
      JSON.stringify({
        run: {
          session: '1',
          chosen: 'forever.yaml',
          file: 'forever.yaml',
          disable: [],
          manual: [],
          speed: 1,
          stopAt: 'abc',
          stopAtEdited: true,
          stopSession: null,
          scenario: firstScenario,
          afterScenario: 'pause',
          logSource: '',
        },
      }),
    );
    const appBase = response('app') as { launcher: { version: { features: string[] } } };
    const appWithHandover = {
      ...appBase,
      launcher: {
        ...appBase.launcher,
        version: {
          ...appBase.launcher.version,
          features: [...new Set([...appBase.launcher.version.features, 'scenario-handover'])],
        },
      },
    };
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithHandover);
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    // ws-pass[3] has session id '1' and file 'simulation.yaml' (!== 'forever.yaml')
    const endedSummary = structuredClone(server('ws-pass')[3]) as {
      session: {
        state: string;
        ended_at: string;
        launcher: { running: boolean };
        allowed: { start: { ok: boolean; reason: string | null; message: string | null } };
      };
    };
    endedSummary.session.state = 'ended';
    endedSummary.session.ended_at = '2026-01-01T00:00:01Z';
    endedSummary.session.launcher.running = false;
    endedSummary.session.allowed.start = { ok: true, reason: null, message: null };
    fake.last().send(endedSummary);
    fake.last().send({ type: 'synced', stream_seq: 99, run_id: '1', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();

    const checkReq = await checkRequest();
    expect(checkReq.request.body).toEqual({
      file: 'forever.yaml',
      disable: [],
      manual: [],
      scenario: firstScenario,
      after_scenario: 'pause',
      speed: 1,
    });
    expect((checkReq.request.body as Record<string, unknown>)['stop_at_ms']).toBeUndefined();
    checkReq.flush(response('check-passed'));
    await advance(1);
    await settle();

    const pauseRadio = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="after-scenario-pause"] input[type="radio"]',
    )!;
    const stopInput = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="stop-at-ms"]',
    )!;
    expect(pauseRadio.checked).toBe(true);
    expect(stopInput.disabled).toBe(true);
    expect(stopInput.value).toBe('');
    expect(text(fixture, 'blocked')).toBe('');
    expect(disabled(button(fixture, 'start'))).toBe(false);
    expect(pageIntent.get()).toMatchObject({
      scenario: firstScenario,
      after_scenario: 'pause',
    });
    expect(pageIntent.get().stop_at_ms).toBeUndefined();
  });

  it('shows calm guidance for variant and case usage, sends typed params, and restores session.intent on session switch', async () => {
    const appBase = response('app') as { launcher: { version: { features: string[] } } };
    const appWithHandover = {
      ...appBase,
      launcher: {
        ...appBase.launcher,
        version: {
          ...appBase.launcher.version,
          features: [...new Set([...appBase.launcher.version.features, 'scenario-handover'])],
        },
      },
    };
    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithHandover);
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();

    // Variant usage check shows calm guidance without red error count
    const baseCheck = structuredClone(response('check-passed')) as {
      check: Record<string, unknown>;
    };
    (await checkRequest()).flush({
      check: {
        ...baseCheck.check,
        result: 'usage',
        errors: 1,
        variant: null,
        variants: ['small', 'big'],
        findings: [
          {
            severity: 'error',
            code: 'usage',
            file: 'bad-lines.yaml',
            line: 1,
            participant: null,
            network: null,
            message: 'choose a variant',
            detail: null,
          },
        ],
      },
    });
    await settle();
    expect(text(fixture, 'check-outcome')).toContain('バリアントを選んでください。');
    expect(text(fixture, 'check-outcome')).not.toContain('誤り');
    expect(text(fixture, 'blocked')).toContain('バリアントを選んでください。');
    expect(
      (fixture.nativeElement as HTMLElement)
        .querySelector('[data-testid="check-outcome"]')
        ?.classList.contains('failed'),
    ).toBe(false);

    // Choose variant + scenario -> case usage check shows calm case guidance
    const page = fixture.componentInstance as unknown as {
      selectVariant(v: string | null): void;
      selectScenario(s: string | null): void;
      selectCase(c: string | null): void;
    };
    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    page.selectVariant('small');
    page.selectScenario(firstScenario);
    TestBed.tick();
    const caseUsageCheck = await checkRequest();
    expect(caseUsageCheck.request.body).toMatchObject({
      variant: 'small',
      scenario: firstScenario,
    });
    caseUsageCheck.flush({
      check: {
        ...baseCheck.check,
        result: 'usage',
        errors: 1,
        variant: 'small',
        variants: ['small', 'big'],
        findings: [
          {
            severity: 'error',
            code: 'usage',
            file: 'bad-lines.yaml',
            line: null,
            participant: null,
            network: null,
            message: 'choose a case',
            detail: null,
          },
        ],
        scenario: {
          file: firstScenario,
          name: 'cases',
          case: null,
          cases: ['c1', 'big_only'],
          case_details: [
            { name: 'c1', only: null },
            { name: 'big_only', only: ['big'] },
          ],
          steps: 0,
          params: [
            { name: 'throttle', value: 50, from: 'scenario', type: 'integer' },
            { name: 'tag', value: '0001', from: 'scenario', type: 'string' },
          ],
          param_meta: [
            { name: 'throttle', type: 'integer' },
            { name: 'tag', type: 'string' },
          ],
        },
      },
    });
    await settle();
    expect(text(fixture, 'check-outcome')).toContain('シナリオのケースを選んでください。');
    expect(text(fixture, 'blocked')).toContain('シナリオのケースを選んでください。');

    // Select case and edit typed params ("0123" stays string, "75" becomes number)
    page.selectCase('c1');
    TestBed.tick();
    const tagInput = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="param-input-tag"]',
    )!;
    tagInput.value = '0123';
    tagInput.dispatchEvent(new Event('input'));
    const thrInput = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="param-input-throttle"]',
    )!;
    thrInput.value = '75';
    thrInput.dispatchEvent(new Event('input'));
    TestBed.tick();

    const typedCheck = await checkRequest();
    expect(typedCheck.request.body).toMatchObject({
      variant: 'small',
      scenario: firstScenario,
      case: 'c1',
      params: { tag: '0123', throttle: 75 },
    });
    expect(
      typeof (typedCheck.request.body as { params: Record<string, unknown> }).params['tag'],
    ).toBe('string');
    expect(
      typeof (typedCheck.request.body as { params: Record<string, unknown> }).params['throttle'],
    ).toBe('number');
    typedCheck.flush({
      check: {
        ...baseCheck.check,
        result: 'passed',
        variant: 'small',
        variants: ['small', 'big'],
        scenario: {
          file: firstScenario,
          name: 'cases',
          case: 'c1',
          cases: ['c1', 'big_only'],
          case_details: [
            { name: 'c1', only: null },
            { name: 'big_only', only: ['big'] },
          ],
          steps: 2,
          params: [
            { name: 'throttle', value: 75, from: 'request_file', type: 'integer' },
            { name: 'tag', value: '0123', from: 'request_file', type: 'string' },
          ],
        },
      },
    });
    await settle();

    // Switching to an ended session with session.intent restores after_scenario='pause', variant, case, params,
    // and clicking rerun sends session.intent directly to POST /api/run
    const baseSession = (server('ws-pass')[3] as { session: Record<string, unknown> }).session;
    const endedIntent = {
      disable: [],
      manual: [],
      variant: 'small',
      scenario: firstScenario,
      case: 'c1',
      params: { tag: '0123', throttle: 75 },
      after_scenario: 'pause',
      speed: 'max',
    };
    fake.last().send({
      type: 'session',
      stream_seq: 50,
      session: {
        ...baseSession,
        id: '42',
        file: 'bad-lines.yaml',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        allowed: { start: { ok: true, reason: null, message: null } },
        intent: endedIntent,
        scenario: {
          file: firstScenario,
          name: 'cases',
          case: 'c1',
          params: [
            { name: 'throttle', value: 75, from: 'request_file' },
            { name: 'tag', value: '0123', from: 'request_file' },
          ],
          step_count: 1,
          steps: [
            {
              index: 1,
              line: 5,
              kind: 'set',
              text: 'set Engine.engine.throttle = 75',
              status: 'passed',
              start_ms: 0,
              at_ms: 0,
              end_ms: 0,
              code: null,
              message: null,
            },
          ],
          steps_omitted: 0,
          status: 'passed',
          over_at_ms: 100,
          timeline_url: null,
        },
        result: {
          vehicle_sil_run: 2,
          file: 'bad-lines.yaml',
          result: 'passed',
          end: 'completed',
          ok: true,
          simulation: 'ended',
          interrupted_by: null,
          errors: 0,
          warnings: 0,
          notes: 0,
          participants: [],
          findings: [],
          timing: null,
          scenario: {
            file: firstScenario,
            name: 'cases',
            case: 'c1',
            params: [
              { name: 'throttle', value: 75, from: 'request_file' },
              { name: 'tag', value: '0123', from: 'request_file' },
            ],
            status: 'passed',
            over_at_ms: 100,
            steps: [],
          },
        },
      },
    });
    fake.last().send({ type: 'synced', stream_seq: 51, run_id: '42', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    await advance(CHECK_DELAY_MS);
    for (const req of http.match('api/check')) req.flush(response('check-passed'));
    answerScenarios();
    await settle();

    // afterScenario was restored to 'pause' from session.intent (not reset to 'stop')
    const pauseRadio = (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
      '[data-testid="after-scenario-pause"] input[type="radio"]',
    )!;
    expect(pauseRadio.checked).toBe(true);

    // Clicking rerun sends session.intent directly to POST /api/run
    button(fixture, 'scenario-rerun').click();
    const rerunReq = http.expectOne('api/run');
    expect(rerunReq.request.body).toEqual({
      file: 'bad-lines.yaml',
      ...endedIntent,
    });
    rerunReq.flush(response('run'));
  });

  it('clears parameter overrides and invalid drafts on case switch, variant case invalidation, and external session sync', async () => {
    const fixture = await openWithFiles();
    const baseCheck = structuredClone(response('check-passed')) as {
      check: Record<string, unknown>;
    };
    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    const makeCheck = (
      variant: string,
      scenarioCase: string | null,
      throttle: number,
      result: 'passed' | 'usage' = 'passed',
    ) => ({
      check: {
        ...baseCheck.check,
        result,
        errors: result === 'usage' ? 1 : 0,
        variant,
        variants: ['small', 'big'],
        scenario: {
          file: firstScenario,
          name: 'cases',
          case: scenarioCase,
          cases: ['case_a', 'case_b', 'big_only'],
          case_details: [
            { name: 'case_a', only: null },
            { name: 'case_b', only: null },
            { name: 'big_only', only: ['big'] },
          ],
          steps: 2,
          params: [{ name: 'throttle', value: throttle, from: 'case', type: 'integer' }],
          param_meta: [{ name: 'throttle', type: 'integer' }],
        },
      },
    });
    (await checkRequest()).flush(makeCheck('big', null, 40, 'usage'));
    await settle();

    const page = fixture.componentInstance as unknown as {
      selectVariant(v: string | null): void;
      selectScenario(s: string | null): void;
      selectCase(c: string | null): void;
    };
    const thrInput = () =>
      (fixture.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
        '[data-testid="param-input-throttle"]',
      )!;

    page.selectVariant('big');
    page.selectScenario(firstScenario);
    page.selectCase('case_a');
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('big', 'case_a', 40));
    await settle();

    // Override throttle in case_a, then switch to case_b -> overrides must not leak
    thrInput().value = '55';
    thrInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    const overrideCheck = await checkRequest();
    expect(overrideCheck.request.body).toMatchObject({
      variant: 'big',
      case: 'case_a',
      params: { throttle: 55 },
    });
    overrideCheck.flush(makeCheck('big', 'case_a', 55));
    await settle();

    page.selectCase('case_b');
    TestBed.tick();
    const caseBCheck = await checkRequest();
    expect((caseBCheck.request.body as Record<string, unknown>)['params']).toBeUndefined();
    expect(caseBCheck.request.body).toMatchObject({ case: 'case_b' });
    caseBCheck.flush(makeCheck('big', 'case_b', 80));
    await settle();
    expect(thrInput().value).toBe('80');

    // Type invalid integer in case_b, then switch back to case_a -> invalid draft must clear
    thrInput().value = '12.5';
    thrInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(text(fixture, 'blocked')).toContain('パラメータの入力値を確認してください。');

    page.selectCase('case_a');
    TestBed.tick();
    const caseACheck = await checkRequest();
    expect((caseACheck.request.body as Record<string, unknown>)['params']).toBeUndefined();
    caseACheck.flush(makeCheck('big', 'case_a', 40));
    await settle();
    expect(thrInput().value).toBe('40');
    expect(text(fixture, 'blocked')).toBe('');

    // Select big_only + override throttle, then switch variant to small -> both case and params clear
    page.selectCase('big_only');
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('big', 'big_only', 90));
    await settle();

    thrInput().value = '88';
    thrInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('big', 'big_only', 88));
    await settle();

    page.selectVariant('small');
    TestBed.tick();
    const smallCheck = await checkRequest();
    expect((smallCheck.request.body as Record<string, unknown>)['case']).toBeUndefined();
    expect((smallCheck.request.body as Record<string, unknown>)['params']).toBeUndefined();
    smallCheck.flush(makeCheck('small', null, 40, 'usage'));
    await settle();

    // External session update while idle refreshes input display to the new intent params
    page.selectCase('case_a');
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('small', 'case_a', 40));
    await settle();
    thrInput().value = '55';
    thrInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('small', 'case_a', 55));
    await settle();

    const baseSession = (server('ws-pass')[3] as { session: Record<string, unknown> }).session;
    fake.last().send({
      type: 'session',
      stream_seq: 60,
      session: {
        ...baseSession,
        id: '99',
        file: 'bad-lines.yaml',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        allowed: { start: { ok: true, reason: null, message: null } },
        intent: {
          disable: [],
          manual: [],
          variant: 'small',
          scenario: firstScenario,
          case: 'case_a',
          params: { throttle: 77 },
          speed: 1,
        },
      },
    });
    fake.last().send({ type: 'synced', stream_seq: 61, run_id: '99', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    (await checkRequest()).flush(makeCheck('small', 'case_a', 77));
    await settle();
    expect(thrInput().value).toBe('77');

    // Switching to case_b after an ended case_a session does not apply case_a's session.intent.params
    page.selectCase('case_b');
    TestBed.tick();
    const afterSessionCaseB = await checkRequest();
    expect((afterSessionCaseB.request.body as Record<string, unknown>)['params']).toBeUndefined();
    afterSessionCaseB.flush(makeCheck('small', 'case_b', 80));
    await settle();
    expect(thrInput().value).toBe('80');
  });

  it('shows Recorded signals only with a scenario when record-signals is advertised, resets drafts on file switch, and preserves cleared record across language switch', async () => {
    const appWithRecord = () => {
      const base = response('app') as { launcher: { version: { features: string[] } } };
      return {
        ...base,
        launcher: {
          ...base.launcher,
          version: {
            ...base.launcher.version,
            features: [...new Set([...base.launcher.version.features, 'record-signals'])],
          },
        },
      };
    };
    const checkWithVars = () => {
      const base = response('check-passed') as {
        check: { participants: Record<string, unknown>[] };
      };
      return {
        ...base,
        check: {
          ...base.check,
          participants: [
            {
              name: 'Engine',
              enabled: true,
              manual: false,
              required: true,
              status: 'described',
              step_ms: 10,
              variables: [
                {
                  name: 'engine.demand',
                  type: 'real',
                  causality: 'input',
                  unit: '%',
                  min: 0,
                  max: 100,
                  builtin: false,
                },
                {
                  name: 'engine.mode',
                  type: 'enum',
                  causality: 'parameter',
                  unit: '',
                  min: null,
                  max: null,
                  builtin: false,
                },
              ],
            },
            {
              name: 'Dashboard',
              enabled: true,
              manual: false,
              required: true,
              status: 'described',
              step_ms: 5,
              variables: [
                {
                  name: 'dashboard.speed',
                  type: 'integer',
                  causality: 'output',
                  unit: 'rpm',
                  min: null,
                  max: null,
                  builtin: false,
                },
              ],
            },
          ],
        },
      };
    };

    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    const secondFile = (response('configs') as { files: { path: string }[] }).files[1]?.path;

    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithRecord());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();
    const initialCheck = await checkRequest();
    expect((initialCheck.request.body as Record<string, unknown>)['record']).toBeUndefined();
    initialCheck.flush(checkWithVars());
    await settle();

    const host = fixture.nativeElement as HTMLElement;
    // Hidden for manual runs (no scenario selected)
    expect(host.querySelector('[data-testid="record-signals"]')).toBeNull();

    const page = fixture.componentInstance as unknown as {
      selectFile(f: string): void;
      selectScenario(s: string | null): void;
    };
    page.selectScenario(firstScenario);
    TestBed.tick();
    (await checkRequest()).flush(checkWithVars());
    await settle();

    const details = host.querySelector<HTMLDetailsElement>('[data-testid="record-signals"]');
    expect(details).not.toBeNull();
    expect(details!.open).toBe(false);
    expect(host.querySelector('[data-testid="record-signals-count"]')).toBeNull();

    // Open disclosure, type into filter & custom drafts, and pick Dashboard.dashboard.speed
    details!.open = true;
    TestBed.tick();
    const filterInput = host.querySelector<HTMLInputElement>(
      '[data-testid="record-signal-filter"]',
    )!;
    filterInput.value = 'dashboard';
    filterInput.dispatchEvent(new Event('input'));
    const customInput = host.querySelector<HTMLInputElement>(
      '[data-testid="record-signal-custom"]',
    )!;
    customInput.value = 'Engine.draft';
    customInput.dispatchEvent(new Event('input'));
    TestBed.tick();
    const opt = host.querySelector<HTMLInputElement>(
      '[data-testid="record-signal-option-Dashboard.dashboard.speed"] input[type="checkbox"]',
    )!;
    opt.click();
    TestBed.tick();
    expect(host.querySelector('[data-testid="record-signals-count"]')?.textContent).toContain('1');
    expect(pageIntent.get().record).toEqual(['Dashboard.dashboard.speed']);

    const recCheck = await checkRequest();
    expect(recCheck.request.body).toMatchObject({
      scenario: firstScenario,
      record: ['Dashboard.dashboard.speed'],
    });
    recCheck.flush(checkWithVars());
    await advance(1);
    await settle();

    button(fixture, 'start').click();
    const runReq = http.expectOne('api/run');
    expect(runReq.request.body).toMatchObject({
      scenario: firstScenario,
      record: ['Dashboard.dashboard.speed'],
    });
    runReq.flush(response('run'));

    // Switching back to manual hides selector and omits record from check/pageIntent
    page.selectScenario(null);
    TestBed.tick();
    expect(host.querySelector('[data-testid="record-signals"]')).toBeNull();
    expect(pageIntent.get().record).toBeUndefined();
    const manualCheck = await checkRequest();
    expect((manualCheck.request.body as Record<string, unknown>)['record']).toBeUndefined();
    manualCheck.flush(checkWithVars());
    await settle();

    // Switching config file resets filter and custom draft inputs inside RecordSignals
    if (secondFile) {
      page.selectScenario(firstScenario);
      TestBed.tick();
      (await checkRequest()).flush(checkWithVars());
      await settle();
      const d2 = host.querySelector<HTMLDetailsElement>('[data-testid="record-signals"]')!;
      d2.open = true;
      TestBed.tick();
      const f2 = host.querySelector<HTMLInputElement>('[data-testid="record-signal-filter"]')!;
      f2.value = 'speed';
      f2.dispatchEvent(new Event('input'));
      const c2 = host.querySelector<HTMLInputElement>('[data-testid="record-signal-custom"]')!;
      c2.value = 'Engine.custom';
      c2.dispatchEvent(new Event('input'));
      TestBed.tick();
      page.selectFile(secondFile);
      TestBed.tick();
      (await checkRequest()).flush(checkWithVars());
      await settle();
      page.selectScenario(firstScenario);
      TestBed.tick();
      (await checkRequest()).flush(checkWithVars());
      await settle();
      const d3 = host.querySelector<HTMLDetailsElement>('[data-testid="record-signals"]')!;
      d3.open = true;
      TestBed.tick();
      expect(
        host.querySelector<HTMLInputElement>('[data-testid="record-signal-filter"]')!.value,
      ).toBe('');
      expect(
        host.querySelector<HTMLInputElement>('[data-testid="record-signal-custom"]')!.value,
      ).toBe('');
      page.selectFile('bad-lines.yaml');
      TestBed.tick();
      (await checkRequest()).flush(checkWithVars());
      await settle();
    }

    // Ended session with intent.scenario + intent.record restores selected signals
    const baseSession = (server('ws-pass')[3] as { session: Record<string, unknown> }).session;
    const endedSessionMsg = {
      type: 'session',
      stream_seq: 40,
      session: {
        ...baseSession,
        id: '77',
        file: 'bad-lines.yaml',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        allowed: { start: { ok: true, reason: null, message: null } },
        intent: {
          disable: [],
          manual: [],
          scenario: firstScenario,
          record: ['Engine.engine.mode'],
          speed: 1,
        },
      },
    };
    fake.last().send(endedSessionMsg);
    fake.last().send({ type: 'synced', stream_seq: 41, run_id: '77', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    const restoredCheck = await checkRequest();
    expect(restoredCheck.request.body).toMatchObject({
      scenario: firstScenario,
      record: ['Engine.engine.mode'],
    });
    restoredCheck.flush(checkWithVars());
    await settle();

    // Clear all omits record from intent and next check, and persists record: [] across language switch
    const detailsRestored = host.querySelector<HTMLDetailsElement>(
      '[data-testid="record-signals"]',
    )!;
    detailsRestored.open = true;
    TestBed.tick();
    host.querySelector<HTMLButtonElement>('[data-testid="record-signals-clear"]')!.click();
    TestBed.tick();
    expect(pageIntent.get().record).toBeUndefined();
    const clearedCheck = await checkRequest();
    expect((clearedCheck.request.body as Record<string, unknown>)['record']).toBeUndefined();
    clearedCheck.flush(checkWithVars());
    await settle();

    // Simulate JA -> EN language switch carryover on the SAME ended session whose intent.record is non-empty
    const lang = TestBed.inject(LanguageChoice);
    (lang as unknown as { reload: () => void }).reload = vi.fn();
    lang.choose('en');
    fixture.destroy();
    TestBed.resetTestingModule();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [RunPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        { provide: ACTION_GUARD, useValue: 0 },
        { provide: PAGE_RELOAD, useValue: vi.fn() },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
    const reloaded = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithRecord());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send({
      ...(server('ws-pass')[0] as Record<string, unknown>),
      session: endedSessionMsg.session,
    });
    fake.last().send({ type: 'synced', stream_seq: 1, run_id: '77', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    const afterLangCheck = await checkRequest();
    expect((afterLangCheck.request.body as Record<string, unknown>)['record']).toBeUndefined();
    afterLangCheck.flush(checkWithVars());
    await settle();
    expect(
      (reloaded.nativeElement as HTMLElement).querySelector('[data-testid="record-signals-count"]'),
    ).toBeNull();
    localStorage.removeItem('vehicle-sil-gui-language');
  });

  it('shows Record after failure input only with a scenario when record-after-fail is advertised, validates 1..600000, resets on file switch, restores from session.intent, and preserves clearing across language switch', async () => {
    const appWithTail = () => {
      const base = response('app') as { launcher: { version: { features: string[] } } };
      return {
        ...base,
        launcher: {
          ...base.launcher,
          version: {
            ...base.launcher.version,
            features: [...new Set([...base.launcher.version.features, 'record-after-fail'])],
          },
        },
      };
    };

    const firstScenario = (response('scenarios') as { scenarios: { path: string }[] }).scenarios[0]
      .path;
    const secondFile = (response('configs') as { files: { path: string }[] }).files[1]?.path;

    const fixture = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithTail());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send(server('ws-pass')[0]);
    fake.last().send(server('ws-pass')[1]);
    await advance(BATCH_MS);
    TestBed.tick();
    const initialCheck = await checkRequest();
    expect(
      (initialCheck.request.body as Record<string, unknown>)['record_after_fail_ms'],
    ).toBeUndefined();
    initialCheck.flush(response('check-passed'));
    await settle();

    const host = fixture.nativeElement as HTMLElement;
    // Hidden without a scenario
    expect(host.querySelector('[data-testid="record-after-fail-ms"]')).toBeNull();

    const page = fixture.componentInstance as unknown as {
      selectFile(f: string): void;
      selectScenario(s: string | null): void;
    };
    page.selectScenario(firstScenario);
    TestBed.tick();
    (await checkRequest()).flush(response('check-passed'));
    await settle();

    const tailInput = () =>
      host.querySelector<HTMLInputElement>('[data-testid="record-after-fail-ms"]')!;
    expect(tailInput()).not.toBeNull();
    expect(tailInput().value).toBe('');

    // Reject 0 and > 600000
    tailInput().value = '0';
    tailInput().dispatchEvent(new Event('input'));
    tailInput().dispatchEvent(new Event('blur'));
    TestBed.tick();
    expect(text(fixture, 'blocked')).toContain('失敗後の追加記録は 1 から 600000 までの整数');

    tailInput().value = '600001';
    tailInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(text(fixture, 'blocked')).toContain('失敗後の追加記録は 1 から 600000 までの整数');

    // Valid value 300 is forwarded in check, pageIntent, and run
    tailInput().value = '300';
    tailInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(pageIntent.get().record_after_fail_ms).toBe(300);

    const tailCheck = await checkRequest();
    expect(tailCheck.request.body).toMatchObject({
      scenario: firstScenario,
      record_after_fail_ms: 300,
    });
    tailCheck.flush(response('check-passed'));
    await advance(1);
    await settle();
    expect(text(fixture, 'blocked')).toBe('');

    button(fixture, 'start').click();
    const runReq = http.expectOne('api/run');
    expect(runReq.request.body).toMatchObject({
      scenario: firstScenario,
      record_after_fail_ms: 300,
    });
    runReq.flush(response('run'));

    // Switching config file resets recordAfterFailMs
    if (secondFile) {
      page.selectFile(secondFile);
      TestBed.tick();
      (await checkRequest()).flush(response('check-passed'));
      await settle();
      page.selectScenario(firstScenario);
      TestBed.tick();
      (await checkRequest()).flush(response('check-passed'));
      await settle();
      expect(tailInput().value).toBe('');
      page.selectFile('bad-lines.yaml');
      TestBed.tick();
      (await checkRequest()).flush(response('check-passed'));
      await settle();
    }

    // Ended session with intent.record_after_fail_ms restores '300'
    const baseSession = (server('ws-pass')[3] as { session: Record<string, unknown> }).session;
    const endedSessionMsg = {
      type: 'session',
      stream_seq: 44,
      session: {
        ...baseSession,
        id: '88',
        file: 'bad-lines.yaml',
        state: 'ended',
        launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
        allowed: { start: { ok: true, reason: null, message: null } },
        intent: {
          disable: [],
          manual: [],
          scenario: firstScenario,
          record_after_fail_ms: 300,
          speed: 1,
        },
      },
    };
    fake.last().send(endedSessionMsg);
    fake.last().send({ type: 'synced', stream_seq: 45, run_id: '88', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    const restoredCheck = await checkRequest();
    expect(restoredCheck.request.body).toMatchObject({
      scenario: firstScenario,
      record_after_fail_ms: 300,
    });
    restoredCheck.flush(response('check-passed'));
    await settle();
    expect(tailInput().value).toBe('300');

    // Clearing the field omits record_after_fail_ms and persists empty across JA -> EN language switch
    tailInput().value = '';
    tailInput().dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(pageIntent.get().record_after_fail_ms).toBeUndefined();
    const clearedCheck = await checkRequest();
    expect(
      (clearedCheck.request.body as Record<string, unknown>)['record_after_fail_ms'],
    ).toBeUndefined();
    clearedCheck.flush(response('check-passed'));
    await settle();

    const lang = TestBed.inject(LanguageChoice);
    (lang as unknown as { reload: () => void }).reload = vi.fn();
    lang.choose('en');
    fixture.destroy();
    TestBed.resetTestingModule();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [RunPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        { provide: ACTION_GUARD, useValue: 0 },
        { provide: PAGE_RELOAD, useValue: vi.fn() },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
    const reloaded = TestBed.createComponent(RunPage);
    TestBed.tick();
    http.expectOne('api/app').flush(appWithTail());
    http.expectOne('api/configs').flush(response('configs'));
    fake.last().open();
    fake.last().send({
      ...(server('ws-pass')[0] as Record<string, unknown>),
      session: endedSessionMsg.session,
    });
    fake.last().send({ type: 'synced', stream_seq: 1, run_id: '88', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    const afterLangCheck = await checkRequest();
    expect(
      (afterLangCheck.request.body as Record<string, unknown>)['record_after_fail_ms'],
    ).toBeUndefined();
    afterLangCheck.flush(response('check-passed'));
    await settle();
    expect(
      (reloaded.nativeElement as HTMLElement).querySelector<HTMLInputElement>(
        '[data-testid="record-after-fail-ms"]',
      )!.value,
    ).toBe('');
    localStorage.removeItem('vehicle-sil-gui-language');
  });
});

describe('readRunChoices', () => {
  const choices = {
    session: '3',
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
    expect(readRunChoices({ ...choices, afterScenario: 'pause' })?.afterScenario).toBe('pause');
    expect(readRunChoices({ ...choices, record: ['Engine.engine.mode'] })?.record).toEqual([
      'Engine.engine.mode',
    ]);
    expect(readRunChoices({ ...choices, recordAfterFailMs: '300' })?.recordAfterFailMs).toBe('300');
    expect(readRunChoices({ ...choices, recordAfterFailMs: '' })?.recordAfterFailMs).toBe('');
    expect(readRunChoices({ ...choices, scenario: 'scenarios/a.yaml' })?.scenario).toBe(
      'scenarios/a.yaml',
    );
    expect(
      readRunChoices({
        ...choices,
        variant: 'small',
        case: 'c1',
        params: { code: '0123', throttle: 50, ok: true },
      }),
    ).toMatchObject({
      variant: 'small',
      case: 'c1',
      params: { code: '0123', throttle: 50, ok: true },
    });
  });

  it('carries nothing when the shape is wrong', () => {
    expect(readRunChoices(undefined)).toBeNull();
    expect(readRunChoices({ ...choices, disable: [1] })).toBeNull();
    expect(readRunChoices({ ...choices, speed: 'fast' })).toBeNull();
    expect(readRunChoices({ ...choices, logSource: undefined })).toBeNull();
    expect(readRunChoices({ ...choices, startPaused: 'yes' })).toBeNull();
    expect(readRunChoices({ ...choices, afterScenario: 'resume' })).toBeNull();
    expect(readRunChoices({ ...choices, scenario: 123 })).toBeNull();
    expect(readRunChoices({ ...choices, variant: 123 })).toBeNull();
    expect(readRunChoices({ ...choices, case: 123 })).toBeNull();
    expect(readRunChoices({ ...choices, params: { a: null } })).toBeNull();
    expect(readRunChoices({ ...choices, manual: ['A', 1] })).toBeNull();
    expect(readRunChoices({ ...choices, record: ['A', 1] })).toBeNull();
    expect(readRunChoices({ ...choices, recordAfterFailMs: 300 })).toBeNull();
    expect(readRunChoices({ ...choices, stopAtEdited: true })).toBeNull();
    expect(readRunChoices({ ...choices, stopAtEdited: 'yes' })).toBeNull();
  });
});
