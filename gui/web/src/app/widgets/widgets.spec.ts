import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { Type } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { BATCH_MS, VariableRef } from '@vehicle-sil/gui-client';
import { FakeWebSockets } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { BusConnection } from './bus-connection';
import { GraphWidget } from './graph-widget';
import { LampWidget } from './lamp-widget';
import { NumberWidget } from './number-widget';
import { SelectWidget } from './select-widget';
import { SliderWidget } from './slider-widget';
import { SwitchWidget } from './switch-widget';
import { ValueWidget } from './value-widget';

// 部品の試験: 値と状態の見え方、書き込みの途中・成功・失敗の見え方。サーバーの通知は偽の WebSocket で、
// 書き込みは HttpTestingController で答える。まとめ方と結果の待ち方そのものは gui/client の試験。

const variable = (name: string, fields: object) => ({
  name,
  type: 'real',
  role: 'input',
  writable: true,
  unit: '',
  min: null,
  max: null,
  step: null,
  description: '',
  enum: null,
  ...fields,
});

const SCHEMA = [
  variable('ecu.connected', { type: 'bool' }),
  variable('engine.demand', { unit: '%', min: 0, max: 100 }),
  variable('engine.speed', { type: 'integer', role: 'output', writable: false, unit: 'rpm' }),
  variable('engine.enabled', { type: 'bool' }),
  variable('engine.warning', { type: 'bool', role: 'output', writable: false }),
  variable('engine.mode', {
    type: 'enum',
    role: 'parameter',
    enum: [
      { name: 'off', value: 0 },
      { name: 'on', value: 1 },
    ],
  }),
  variable('engine.offset', { unit: 'rpm' }),
  variable('engine.gear', {
    type: 'enum',
    enum: [
      { name: 'P', value: 0 },
      { name: 'R', value: 1 },
      { name: 'N', value: 2 },
      { name: 'D', value: 3 },
    ],
  }),
];

const variables = (state = 'ready', connected = true) => ({
  state,
  reason: null,
  peers: [{ name: 'Engine', connected, variables: SCHEMA }],
  peers_omitted: 0,
  variables_omitted: 0,
  operations: [],
});

const hello = (state = 'ready', id = 1) => ({
  type: 'hello',
  vehicle_sil_gui: 1,
  version: '0.1.0',
  instance: 'x',
  session: {
    id,
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: variables(state),
  },
  replay: null,
});

const values = (
  sampleMs: number,
  paused: boolean,
  peerValues: Record<string, unknown>,
  session = 1,
) => ({
  type: 'values',
  id: 1,
  session,
  sim_ms: sampleMs,
  paused,
  peers: { Engine: { sample_ms: sampleMs, values: peerValues } },
});

const operation = (seq: number, state: string, done: boolean, peer: object = {}) => ({
  type: 'operation',
  session: 1,
  seq,
  state,
  done,
  peers: { Engine: { state, applied_at_ms: null, late: false, reason: null, ...peer } },
});

const ref = (name: string): VariableRef => ({ peer: 'Engine', name });

describe('variable widgets', () => {
  let fake: FakeWebSockets;
  let http: HttpTestingController;

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

  /** 部品を作り、サーバーの hello を届ける。 */
  async function render<T>(component: Type<T>, name: string, state = 'ready') {
    const fixture = TestBed.createComponent(component);
    fixture.componentRef.setInput('ref', ref(name));
    fixture.detectChanges();
    fake.sockets[0].open();
    await deliver(fixture, hello(state));
    return fixture;
  }

  async function deliver(fixture: ComponentFixture<unknown>, ...messages: unknown[]) {
    for (const message of messages) fake.sockets[0].send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
    fixture.detectChanges();
  }

  const text = (fixture: ComponentFixture<unknown>, testId: string) =>
    (fixture.nativeElement as HTMLElement)
      .querySelector(`[data-testid="${testId}"]`)
      ?.textContent?.replace(/\s+/g, ' ')
      .trim() ?? '';

  const element = <E extends Element = HTMLElement>(
    fixture: ComponentFixture<unknown>,
    css: string,
  ) => (fixture.nativeElement as HTMLElement).querySelector(css) as E;

  const writes = () => http.match('api/write');

  it('shows a value with its unit, and says when it is a value of a stopped time, old or the last of a run', async () => {
    const fixture = await render(ValueWidget, 'engine.speed');
    expect(text(fixture, 'widget-condition')).toBe('… 値はまだありません');
    expect(fake.sockets[0].sent).toContainEqual({
      type: 'watch',
      id: 1,
      variables: [ref('engine.speed')],
      period_ms: 100,
    });
    await deliver(fixture, values(1200, false, { 'engine.speed': 1450 }));
    expect(text(fixture, 'widget-value')).toBe('1450 rpm');
    expect(text(fixture, 'widget-condition')).toBe('1.200 s の値');
    await deliver(fixture, values(1300, true, { 'engine.speed': 1460 }));
    expect(text(fixture, 'widget-condition')).toBe('⏸ 止まっている時刻 1.300 s の値');
    // ECU が切れた: 値は残すが、古いと言う（色だけでなく印と文で）
    await deliver(fixture, {
      type: 'variables',
      session: 1,
      variables: {
        ...variables(),
        peers: [{ name: 'Engine', connected: false, variables: SCHEMA }],
      },
    });
    expect(text(fixture, 'widget-condition')).toBe('⚠ 古い値: ECU がつながっていません');
    await deliver(fixture, { type: 'variables', session: 1, variables: variables('ended', false) });
    expect(text(fixture, 'widget-condition')).toBe('■ 終わった実行の最後の値');
    expect(text(fixture, 'widget-value')).toBe('1460 rpm');
  });

  it('says that a variable is not in this run', async () => {
    const fixture = await render(ValueWidget, 'engine.nothing');
    expect(text(fixture, 'widget-condition')).toBe('⚠ この実行にない変数です');
  });

  it('shows a lamp as on or off in words, not by its colour only', async () => {
    const fixture = await render(LampWidget, 'engine.warning');
    await deliver(fixture, values(100, false, { 'engine.warning': true }));
    expect(text(fixture, 'widget-value')).toBe('オン');
    expect(element(fixture, '[data-testid="widget-value"]').dataset['on']).toBe('true');
    await deliver(fixture, values(200, false, { 'engine.warning': false }));
    expect(text(fixture, 'widget-value')).toBe('オフ');
  });

  it('writes a switch, shows the write on its way, taken and applied, and shows the value sent meanwhile', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    await deliver(fixture, values(100, false, { 'engine.enabled': false }));
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    expect(button.getAttribute('aria-checked')).toBe('false');
    // 名前は変数の名前
    const labelledBy = button.getAttribute('aria-labelledby') ?? '';
    expect(element(fixture, `[id="${labelledBy}"]`)?.textContent?.trim()).toBe('engine.enabled');
    button.click();
    fixture.detectChanges();
    const [request] = writes();
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'engine.enabled': true } },
    });
    expect(text(fixture, 'widget-write')).toBe('… 送っています');
    expect(button.getAttribute('aria-checked')).toBe('true');
    request.flush({ session_id: 1, operation: 5 });
    fixture.detectChanges();
    expect(text(fixture, 'widget-write')).toBe(
      '… 受け付けました。ECU のステップで反映されるのを待っています',
    );
    await deliver(fixture, operation(5, 'applied', true, { applied_at_ms: 200 }));
    await deliver(fixture, values(200, false, { 'engine.enabled': true }));
    expect(text(fixture, 'widget-write')).toBe('✓ 0.200 s のステップで反映しました');
    expect(button.getAttribute('aria-checked')).toBe('true');
    expect(text(fixture, 'widget-error')).toBe('');
  });

  it('the bus connection: a switch named for the ECU, and words and a mark while the ECU is off its buses', async () => {
    const fixture = await render(BusConnection, 'ecu.connected');
    const host = fixture.nativeElement as HTMLElement;
    await deliver(fixture, values(100, false, { 'ecu.connected': true }));
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    expect(button.getAttribute('aria-checked')).toBe('true');
    expect(button.getAttribute('aria-label')).toBe('Engine をバスにつなぐ');
    expect(host.dataset['connected']).toBe('true');
    expect(text(fixture, 'bus-state')).toBe('');
    button.click();
    fixture.detectChanges();
    const [request] = writes();
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'ecu.connected': false } },
    });
    request.flush({ session_id: 1, operation: 5 });
    await deliver(fixture, operation(5, 'applied', true, { applied_at_ms: 200 }));
    await deliver(fixture, values(200, false, { 'ecu.connected': false }));
    expect(host.dataset['connected']).toBe('false');
    expect(button.getAttribute('aria-checked')).toBe('false');
    expect(text(fixture, 'bus-state')).toBe(
      '⚠この ECU はバスから外れています（CAN と LIN に送らず、受けません。ステップは動き続けます）。',
    );
  });

  it('shows why a switch was refused, as an alert, and goes back to the value read', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    await deliver(fixture, values(100, false, { 'engine.enabled': false }));
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    button.click();
    fixture.detectChanges();
    expect(button.getAttribute('aria-checked')).toBe('true');
    writes()[0].flush(
      { error: { code: 'not-running', message: 'the run is not running', detail: null } },
      { status: 409, statusText: 'Conflict' },
    );
    await vi.advanceTimersByTimeAsync(0);
    fixture.detectChanges();
    expect(text(fixture, 'widget-error')).toBe('✗ サーバーが断りました: the run is not running');
    expect(element(fixture, '[data-testid="widget-error"]').getAttribute('role')).toBe('alert');
    expect(element(fixture, '[data-testid="widget-error"] [lang="en"]')).not.toBeNull();
    expect(button.getAttribute('aria-checked')).toBe('false');
  });

  it('shows a write that was not applied', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    element<HTMLButtonElement>(fixture, 'button[role="switch"]').click();
    writes()[0].flush({ session_id: 1, operation: 3 });
    await deliver(fixture, operation(3, 'late', true, { reason: 'its time had passed' }));
    expect(text(fixture, 'widget-error')).toBe(
      '✗ 反映されませんでした: the write (operation 3) was not applied: Engine: late (its time had passed)',
    );
  });

  it('sends only the last position of a slider let go while the write before it is not taken yet', async () => {
    const fixture = await render(SliderWidget, 'engine.demand');
    await deliver(fixture, values(100, false, { 'engine.demand': 20 }));
    expect(text(fixture, 'widget-value')).toBe('20.00 %');
    const thumb = element<HTMLInputElement>(fixture, 'input[type="range"]');
    expect(thumb.getAttribute('aria-labelledby')).toMatch(/-name$/);
    const release = (value: number) => {
      thumb.value = String(value);
      thumb.dispatchEvent(new Event('input'));
      thumb.dispatchEvent(new Event('change'));
      fixture.detectChanges();
    };
    // 動かしている間は書かない
    thumb.value = '30';
    thumb.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(writes()).toHaveLength(0);
    expect(text(fixture, 'widget-value')).toBe('30.00 %');
    release(40);
    release(50);
    release(60);
    const first = writes();
    expect(first.map((r) => r.request.body)).toEqual([
      { session: 1, writes: { Engine: { 'engine.demand': 40 } } },
    ]);
    first[0].flush({ session_id: 1, operation: 1 });
    const second = writes();
    expect(second.map((r) => r.request.body)).toEqual([
      { session: 1, writes: { Engine: { 'engine.demand': 60 } } },
    ]);
    second[0].flush({ session_id: 1, operation: 2 });
    await deliver(
      fixture,
      operation(1, 'applied', true, { applied_at_ms: 200 }),
      operation(2, 'applied', true, { applied_at_ms: 200 }),
    );
    expect(text(fixture, 'widget-write')).toBe('✓ 0.200 s のステップで反映しました');
    expect(writes()).toHaveLength(0);
  });

  it('checks a number before it writes it, and does not send again a write that got no answer', async () => {
    const fixture = await render(NumberWidget, 'engine.offset');
    const field = element<HTMLInputElement>(fixture, 'input[data-testid="widget-control"]');
    const submit = element<HTMLButtonElement>(fixture, '[data-testid="widget-submit"]');
    field.value = 'abc';
    field.dispatchEvent(new Event('input'));
    submit.click();
    fixture.detectChanges();
    expect(text(fixture, 'widget-error')).toBe('✗ 数を入れてください');
    expect(writes()).toHaveLength(0);
    field.value = '12.5';
    field.dispatchEvent(new Event('input'));
    field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' }));
    fixture.detectChanges();
    const [request] = writes();
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'engine.offset': 12.5 } },
    });
    request.flush(
      {
        error: {
          code: 'write-timeout',
          message: 'the control server did not answer in time',
          detail: null,
        },
      },
      { status: 504, statusText: 'Gateway Timeout' },
    );
    await vi.advanceTimersByTimeAsync(60_000);
    fixture.detectChanges();
    expect(text(fixture, 'widget-error')).toMatch(
      /^\? 答えが来ませんでした。反映されたかわかりません（送り直していません）。/,
    );
    expect(writes()).toHaveLength(0);
  });

  it('keeps a number in its range', async () => {
    const fixture = await render(NumberWidget, 'engine.demand');
    const field = element<HTMLInputElement>(fixture, 'input[data-testid="widget-control"]');
    field.value = '101';
    field.dispatchEvent(new Event('input'));
    field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' }));
    fixture.detectChanges();
    expect(text(fixture, 'widget-error')).toBe('✗ 100 以下にしてください');
    expect(writes()).toHaveLength(0);
  });

  it('does not let a parameter be written after the first step, and says why', async () => {
    const fixture = await render(SelectWidget, 'engine.mode');
    await deliver(fixture, values(100, false, { 'engine.mode': 1 }));
    const select = element(fixture, 'mat-select');
    expect(select.getAttribute('aria-disabled')).toBe('true');
    expect(text(fixture, 'widget-why')).toBe(
      'パラメーターは、止めて始めた実行の最初のステップの前だけ書けます',
    );
    expect(select.textContent).toContain('on');
  });

  it('does not let anything be written when the run has ended', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled', 'ended');
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(text(fixture, 'widget-why')).toBe('実行が終わったので書けません');
    button.click();
    expect(writes()).toHaveLength(0);
  });

  it('draws the history of a number', async () => {
    const fixture = await render(GraphWidget, 'engine.speed');
    expect(fake.sockets[0].sent).toContainEqual(
      expect.objectContaining({ type: 'watch', variables: [ref('engine.speed')] }),
    );
    for (const [ms, rpm] of [
      [100, 800],
      [200, 900],
      [300, 1000],
    ]) {
      await deliver(fixture, values(ms, false, { 'engine.speed': rpm }));
    }
    const svg = element<SVGElement>(fixture, 'svg');
    expect(svg.dataset['points']).toBe('3');
    expect(svg.getAttribute('aria-label')).toBe('グラフ: 最後の値 1000 rpm、3 点、0.1 から 0.3 秒');
    expect(element(fixture, 'polyline').getAttribute('points')).toBe(
      '0.0,100.0 150.0,50.0 300.0,0.0',
    );
  });

  it('says a value is not a number, not that there is none', async () => {
    const fixture = await render(ValueWidget, 'engine.speed');
    await deliver(fixture, values(100, false, { 'engine.speed': null }));
    expect(text(fixture, 'widget-value')).toBe('数でない値 rpm');
  });

  it('does not write while the ECU is not connected, and says why', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    await deliver(fixture, {
      type: 'variables',
      session: 1,
      variables: variables('ready', false),
    });
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(text(fixture, 'widget-why')).toBe('ECU がつながっていないので書けません');
    button.click();
    expect(writes()).toHaveLength(0);
  });

  it('does not let a slider move without a value, nor on a variable without a range', async () => {
    const waiting = await render(SliderWidget, 'engine.demand');
    expect(text(waiting, 'widget-why')).toBe(
      '値がまだないので動かせません（値が届くと動かせます）',
    );
    expect(text(waiting, 'widget-value')).toBe('— %');
    const thumb = element<HTMLInputElement>(waiting, 'input[type="range"]');
    expect(thumb.disabled).toBe(true);
    thumb.dispatchEvent(new Event('change'));
    expect(writes()).toHaveLength(0);
    await deliver(waiting, values(100, false, { 'engine.demand': 20 }));
    expect(text(waiting, 'widget-why')).toBe('');
    const unbounded = TestBed.createComponent(SliderWidget);
    unbounded.componentRef.setInput('ref', ref('engine.offset'));
    unbounded.detectChanges();
    expect(text(unbounded, 'widget-why')).toMatch(/^範囲（最小と最大）のない変数なので/);
  });

  it('shows the value read again when a slider cannot be written any more while it is moved', async () => {
    const fixture = await render(SliderWidget, 'engine.demand');
    await deliver(fixture, values(100, false, { 'engine.demand': 20 }));
    const thumb = element<HTMLInputElement>(fixture, 'input[type="range"]');
    thumb.value = '70';
    thumb.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(text(fixture, 'widget-value')).toBe('70.00 %');
    // 動かしている途中で実行が終わった: change は来ない
    await deliver(fixture, { type: 'variables', session: 1, variables: variables('ended') });
    expect(text(fixture, 'widget-value')).toBe('20.00 %');
    expect(writes()).toHaveLength(0);
  });

  it('writes a choice only from the open list, not with keys on the closed one', async () => {
    const fixture = await render(SelectWidget, 'engine.gear');
    await deliver(fixture, values(100, false, { 'engine.gear': 0 }));
    const trigger = element(fixture, 'mat-select');
    trigger.focus();
    for (const key of ['ArrowDown', 'ArrowDown']) {
      trigger.dispatchEvent(
        new KeyboardEvent('keydown', { key, keyCode: 40, bubbles: true } as KeyboardEventInit),
      );
      fixture.detectChanges();
    }
    expect(writes()).toHaveLength(0);
    expect(trigger.textContent).toContain('P');
    (trigger.querySelector('.mat-mdc-select-trigger') as HTMLElement).click();
    fixture.detectChanges();
    const option = [...document.querySelectorAll('mat-option')].find(
      (o) => o.textContent?.trim() === 'D',
    ) as HTMLElement;
    option.click();
    fixture.detectChanges();
    expect(writes().map((r) => r.request.body)).toEqual([
      { session: 1, writes: { Engine: { 'engine.gear': 3 } } },
    ]);
  });

  it('keeps one write state per variable: shared by two widgets and kept when a widget is made again', async () => {
    const first = await render(SwitchWidget, 'engine.enabled');
    const second = TestBed.createComponent(SwitchWidget);
    second.componentRef.setInput('ref', ref('engine.enabled'));
    second.detectChanges();
    element<HTMLButtonElement>(first, 'button[role="switch"]').click();
    first.detectChanges();
    second.detectChanges();
    expect(text(second, 'widget-write')).toBe('… 送っています');
    // もう 1 つの部品から押しても、重ねて送らない（受け取られたら最後の値を送る）
    element<HTMLButtonElement>(second, 'button[role="switch"]').click();
    expect(writes()).toHaveLength(1);
    first.destroy();
    const again = TestBed.createComponent(SwitchWidget);
    again.componentRef.setInput('ref', ref('engine.enabled'));
    again.detectChanges();
    expect(text(again, 'widget-write')).toBe('… 送っています');
  });

  it('forgets the writes of a run when the next one starts', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    await deliver(fixture, values(100, false, { 'engine.enabled': false }));
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    button.click();
    fixture.detectChanges();
    writes()[0].flush({ session_id: 1, operation: 4 });
    fixture.detectChanges();
    expect(text(fixture, 'widget-write')).toMatch(/受け付けました/);
    expect(button.getAttribute('aria-checked')).toBe('true');
    await deliver(fixture, hello('ready', 2), values(0, false, { 'engine.enabled': false }, 2));
    expect(text(fixture, 'widget-write')).toBe('');
    expect(text(fixture, 'widget-error')).toBe('');
    expect(button.getAttribute('aria-checked')).toBe('false');
  });

  it('does not send a value kept back while the write before it was taken, once it cannot be written', async () => {
    const fixture = await render(SwitchWidget, 'engine.enabled');
    await deliver(fixture, values(100, false, { 'engine.enabled': false }));
    const button = element<HTMLButtonElement>(fixture, 'button[role="switch"]');
    button.click();
    fixture.detectChanges();
    button.click(); // 前の書き込みを待つ間: 覚えておくだけ
    fixture.detectChanges();
    const [request] = writes();
    await deliver(fixture, { type: 'variables', session: 1, variables: variables('ended') });
    request.flush({ session_id: 1, operation: 4 });
    fixture.detectChanges();
    expect(writes()).toHaveLength(0);
    expect(text(fixture, 'widget-error')).toMatch(
      /^✗ 書けなくなったので、待たせていた値は送っていません/,
    );
  });
});
