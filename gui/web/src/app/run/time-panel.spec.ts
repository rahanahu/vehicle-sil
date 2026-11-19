import { signal } from '@angular/core';
import { Control, FrontControls } from '@vehicle-sil/gui-client';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { OperationFailure } from '../core/session/gui-client';
import { SimulationTime } from '../core/session/simulation-time';
import { TimePanel } from './time-panel';

describe('TimePanel: the step field', () => {
  let fixture: ComponentFixture<TimePanel>;
  let steps: number[];

  beforeEach(async () => {
    steps = [];
    // 一時停止していて、ステップ送りができる時刻
    const time = {
      simMs: signal(0),
      speed: signal(1),
      actualSpeed: signal(null),
      paused: signal(true),
      paced: signal(false),
      behind: signal(false),
      lateness: signal(null),
      lastCommand: signal(null),
      waiting: signal(false),
      controls: signal({
        pause: { ok: false, reason: { code: 'paused', message: 'paused' }, caution: null },
        resume: { ok: true, reason: null, caution: null },
        step: { ok: true, reason: null, caution: null, min_ms: 1, max_ms: 60000 },
        speed: { ok: true, reason: null, caution: null, min: 0.01, max: 1000, fastest: true },
      }),
      failure: signal(null),
      answerTimeoutMs: 5000,
      step: (ms: number) => Promise.resolve(steps.push(ms)).then(() => undefined),
      setSpeed: () => Promise.resolve(),
    };
    TestBed.configureTestingModule({
      providers: [{ provide: SimulationTime, useValue: time }],
    });
    fixture = TestBed.createComponent(TimePanel);
    await fixture.whenStable();
  });

  function field(): HTMLInputElement {
    return fixture.nativeElement.querySelector('[data-testid="step-ms"]');
  }

  function error(): HTMLElement {
    return fixture.nativeElement.querySelector('[data-testid="step-ms-error"]');
  }

  async function type(text: string): Promise<void> {
    field().value = text;
    field().dispatchEvent(new Event('input'));
    await fixture.whenStable();
  }

  async function submit(): Promise<void> {
    field().form!.requestSubmit();
    await fixture.whenStable();
  }

  it('steps on submit (the Enter of the field), full-width digits too, and stays on the page', async () => {
    await type('２５');
    let prevented = false;
    field().form!.addEventListener('submit', (e) => (prevented = e.defaultPrevented));
    await submit();
    expect(steps).toEqual([25]);
    expect(prevented).toBe(true);
  });

  it('does not step on the Enter that confirms an input-method conversion', async () => {
    await type('２５');
    field().dispatchEvent(
      new KeyboardEvent('keydown', { key: 'Enter', isComposing: true, bubbles: true }),
    );
    await fixture.whenStable();
    expect(steps).toEqual([]);
  });

  it('shows an error when the field is left or sent, not while it is typed', async () => {
    await type('0');
    expect(error().textContent?.trim()).toBe('');
    expect(field().getAttribute('aria-invalid')).toBe('false');
    expect(field().hasAttribute('aria-describedby')).toBe(false);

    field().dispatchEvent(new Event('blur'));
    await fixture.whenStable();
    expect(error().textContent).toContain('1 から 60000');
    expect(error().getAttribute('role')).toBe('status');
    expect(field().getAttribute('aria-invalid')).toBe('true');
    expect(field().getAttribute('aria-describedby')).toBe('step-ms-error');

    // 打ち直すと消える（まだ誤りでも）
    await type('1x');
    expect(error().textContent?.trim()).toBe('');
    expect(field().getAttribute('aria-invalid')).toBe('false');

    // 送ると出る。送らない
    await submit();
    expect(error().textContent).toContain('1 から 60000');
    expect(steps).toEqual([]);
  });
});

describe('TimePanel: why an operation cannot be used', () => {
  const yes: Control = { ok: true, reason: null, caution: null };
  const no = (code: string, message: string): Control => ({
    ok: false,
    reason: { code, message },
    caution: null,
  });
  /** 外につないで実時間を保つ実行（ランチャーの `allowed` を正面の口が写したもの）。 */
  const REAL_TIME = 'this run is connected outside the simulation and keeps to real time';
  const realTime = (): FrontControls => ({
    start: no('already-running', 'a run is going'),
    stop: yes,
    force_stop: no('not-stopping', 'ask for a stop first'),
    pause: no('outside-real-time', `${REAL_TIME}: pause is refused`),
    resume: no('not-paused', 'the simulation is not paused'),
    step: { ...no('outside-real-time', `${REAL_TIME}: step is refused`), min_ms: 5, max_ms: 1000 },
    run_until: no('outside-real-time', `${REAL_TIME}: run_until is refused`),
    speed: { ...yes, min: 1, max: 1, fastest: false },
    save: yes,
  });
  let fixture: ComponentFixture<TimePanel>;
  const controls = signal<FrontControls>(realTime());
  const failure = signal<OperationFailure | null>(null);
  const speeds: (number | 'max')[] = [];

  beforeEach(async () => {
    controls.set(realTime());
    failure.set(null);
    speeds.length = 0;
    const time = {
      simMs: signal(1000),
      speed: signal(1),
      actualSpeed: signal(1),
      paused: signal(false),
      paced: signal(true),
      behind: signal(false),
      lateness: signal(null),
      lastCommand: signal(null),
      waiting: signal(false),
      controls,
      failure,
      answerTimeoutMs: 5000,
      step: () => Promise.resolve(),
      setSpeed: (value: number | 'max') =>
        Promise.resolve(speeds.push(value)).then(() => undefined),
    };
    TestBed.configureTestingModule({ providers: [{ provide: SimulationTime, useValue: time }] });
    fixture = TestBed.createComponent(TimePanel);
    await fixture.whenStable();
  });

  const query = (testId: string): HTMLElement | null =>
    (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="${testId}"]`);
  const toggle = (name: string): HTMLButtonElement =>
    [...(fixture.nativeElement as HTMLElement).querySelectorAll('mat-button-toggle button')].find(
      (b) => b.textContent?.trim() === name,
    ) as HTMLButtonElement;
  const described = (element: Element): string =>
    (element.getAttribute('aria-describedby') ?? '')
      .split(' ')
      .map((id) => document.getElementById(id)?.textContent?.trim() ?? '')
      .join(' ');

  it("shows the launcher's sentence for pause and step, in English, and says nothing that would change nothing", async () => {
    const pause = query('pause-hint')!;
    expect(pause.textContent).toContain(`${REAL_TIME}: pause is refused`);
    // 再開の not-paused（押しても何も変わらない）は出さない
    expect(pause.textContent).not.toContain('一時停止していません');
    expect(pause.querySelector('span')?.getAttribute('lang')).toBe('en');
    const stepButton = query('step')!;
    expect(stepButton.getAttribute('aria-disabled')).toBe('true');
    expect(described(stepButton)).toContain(`${REAL_TIME}: step is refused`);
    expect(query('step-hint')!.querySelector('span')?.getAttribute('lang')).toBe('en');
  });

  it('does not let a speed other than 1 be chosen when the range is 1 only, and says why', async () => {
    for (const name of ['最速', '倍率'])
      expect(toggle(name).getAttribute('aria-disabled')).toBe('true');
    expect(toggle('等速').getAttribute('aria-disabled')).not.toBe('true');
    // 選べる速さの文は、選択肢の並びの説明
    const group = (fixture.nativeElement as HTMLElement).querySelector('mat-button-toggle-group')!;
    expect(described(group)).toBe('この実行で選べる速さは等速だけです。');
    // 押しても選ばれない
    toggle('最速').click();
    await fixture.whenStable();
    expect(toggle('最速').getAttribute('aria-checked')).toBe('false');
    expect(toggle('等速').getAttribute('aria-checked')).toBe('true');
    // 「速度を変える」は押せる（速さ 1 は受け付ける）。理由も注意もないので説明はない
    expect(query('set-speed')!.getAttribute('aria-disabled')).not.toBe('true');
    expect(query('speed-hint')).toBeNull();
  });

  it('says the reason and the caution of the speed change, and takes the step range of the controls', async () => {
    const caution = { code: 'outside-not-real-time', message: 'what is connected does not wait' };
    controls.set({
      ...realTime(),
      pause: { ...yes, caution },
      speed: { ...no('busy', 'wait'), min: 0.5, max: 10, fastest: true },
    });
    await fixture.whenStable();
    expect(query('pause-hint')!.textContent).toContain('what is connected does not wait');
    const setSpeed = query('set-speed')!;
    expect(described(setSpeed)).toBe('送った命令へのランチャーの答えを待っています。');
    expect(query('speed-hint')!.querySelector('span')?.hasAttribute('lang')).toBe(false);
    expect(query('speed-limits')).toBeNull();
    controls.set({ ...realTime(), speed: { ...yes, caution, min: 0.5, max: 10, fastest: true } });
    await fixture.whenStable();
    expect(described(setSpeed)).toBe('what is connected does not wait');
    // ステップ送りの量の下限は controls.step.min_ms
    const field = query('step-ms') as HTMLInputElement;
    field.value = '3';
    field.dispatchEvent(new Event('input'));
    field.dispatchEvent(new Event('blur'));
    await fixture.whenStable();
    expect(query('step-ms-error')!.textContent).toContain('5 から 1000');
  });

  it('shows a reason once on the card, leaves the connection to the band, and keeps both in the descriptions', async () => {
    const all = (code: string, message: string): FrontControls => ({
      ...realTime(),
      pause: no(code, message),
      resume: no(code, message),
      step: { ...no(code, message), min_ms: 1, max_ms: 1000 },
      speed: { ...no(code, message), min: 0.5, max: 10, fastest: true },
    });
    const shownText = (testId: string) =>
      [...(query(testId)?.querySelectorAll('span:not(.quiet)') ?? [])]
        .map((s) => s.textContent?.trim())
        .join(' ');
    // 帯が言う接続の理由は、カードに見せない（ボタンの説明には残る）
    controls.set(all('offline', 'the connection to the GUI server is down'));
    await fixture.whenStable();
    expect(query('pause-hint')).toBeNull();
    expect(shownText('step-hint')).toBe('');
    expect(shownText('speed-hint')).toBe('');
    expect(described(query('step')!)).toContain('サーバーとの接続が切れています');
    expect(described(query('set-speed')!)).toContain('サーバーとの接続が切れています');
    // 3 つの操作の同じ文は、カードに 1 回
    const unsupported = 'the launcher has not said yet what it accepts';
    controls.set(all('unsupported', unsupported));
    await fixture.whenStable();
    expect(query('pause-hint')!.textContent?.trim()).toBe(unsupported);
    expect(shownText('step-hint')).toBe('');
    expect(shownText('speed-hint')).toBe('');
    expect(described(query('set-speed')!)).toBe(unsupported);
  });

  it("shows the last time command's failure from status.problem, and not what the screen leaves out", async () => {
    const refused = () => query('time-refused')!;
    failure.set({
      op: 'step',
      problem: { code: 'not-paused', message: 'step works while paused' },
    });
    await fixture.whenStable();
    expect(refused().textContent).toContain('命令を受け付けられませんでした');
    expect(refused().querySelector('span')?.getAttribute('lang')).toBe('en');
    expect(refused().textContent).toContain('step works while paused');
    // 答えがない: 画面の文
    failure.set({
      op: 'speed',
      problem: { code: 'no-answer', message: 'the launcher did not answer' },
    });
    await fixture.whenStable();
    expect(refused().textContent).toContain('ランチャーが命令（speed）に 5 秒たっても答えません');
    expect(refused().querySelector('[lang="en"]')).toBeNull();
    // 別の送り手に割り込まれた、実行が先に終わった、実行の失敗は、カードに出さない
    for (const code of ['interrupted', 'not-running', 'run-failed', 'offline']) {
      failure.set({ op: 'step', problem: { code, message: code } });
      await fixture.whenStable();
      expect(refused().textContent?.trim()).toBe('');
    }
  });
});
