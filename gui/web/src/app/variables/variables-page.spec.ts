import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import {
  ComponentFixture,
  DeferBlockBehavior,
  DeferBlockState,
  TestBed,
} from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { FakeWebSockets, httpExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { provideVisibleNow } from '../testing/visible-now';
import { Visibility } from '../widgets/visibility';
import { VariablesPage } from './variables-page';

describe('VariablesPage', () => {
  let fake: FakeWebSockets;
  let http: HttpTestingController;
  let fixture: ComponentFixture<VariablesPage>;
  let app: object;

  beforeAll(async () => {
    const base = (await httpExamples()).find((e) => e.name === 'app')?.response as Record<
      string,
      unknown
    >;
    app = { ...base, variables: { available: true, reason: null } };
  });

  beforeEach(async () => {
    vi.useFakeTimers();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [VariablesPage],
      deferBlockBehavior: DeferBlockBehavior.Manual,
      providers: [
        provideRouter([]),
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        provideVisibleNow(),
        { provide: Visibility, useValue: { observe: () => () => undefined } },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
    fixture = TestBed.createComponent(VariablesPage);
    TestBed.tick();
  });

  afterEach(() => {
    TestBed.resetTestingModule();
    vi.useRealTimers();
    sessionStorage.clear();
  });

  const advance = (ms: number) => vi.advanceTimersByTimeAsync(ms);
  const text = (testId: string) =>
    (fixture.nativeElement as HTMLElement)
      .querySelector(`[data-testid="${testId}"]`)
      ?.textContent?.replace(/\s+/g, ' ')
      .trim() ?? '';

  const ok = { ok: true, reason: null, message: null, caution: null };
  const refusal = (reason: string, message: string) => ({
    ok: false,
    reason,
    message,
    caution: null,
  });

  function baseHello(
    launcherState: Record<string, unknown>,
    extraSession: Record<string, unknown> = {},
  ) {
    return {
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'inst-1',
      stream_seq: 1,
      session: {
        id: '1',
        run_t0_ms: 0,
        file: 'simulation.yaml',
        disable: [],
        manual: [],
        speed: 'max',
        start_paused: false,
        state: 'running',
        phase: 'running',
        stop: 'none',
        last_seq: 1,
        last_t_ms: 100,
        sim_ms: 100,
        time_omitted_fields: [],
        time: null,
        last_command: null,
        launcher_state: launcherState,
        allowed: {
          start: refusal('already-running', 'running'),
          stop: ok,
          force: ok,
          save: { ...ok, caution: null },
        },
        check: null,
        participants: [],
        findings: [],
        result: null,
        launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
        log: {},
        problems: [],
        problems_omitted: 0,
        scenario: {
          file: 'scenarios/a.yaml',
          name: 'a',
          step_count: 1,
          steps: [],
          steps_omitted: 0,
          status: null,
          over_at_ms: null,
          timeline_url: null,
        },
        variables: {
          state: 'ready',
          reason: null,
          peers: [
            {
              name: 'Engine',
              connected: true,
              variables: [
                {
                  name: 'engine.enabled',
                  type: 'bool',
                  role: 'input',
                  writable: true,
                  builtin: false,
                  unit: '',
                  min: null,
                  max: null,
                  step: null,
                  description: '',
                  enum: null,
                },
              ],
            },
          ],
          peers_omitted: 0,
          variables_omitted: 0,
          operations: [],
        },
        ...extraSession,
      },
      allowed: {
        start: refusal('already-running', 'running'),
        stop: ok,
        force: ok,
        save: { ...ok, caution: null },
      },
      replay: null,
    };
  }

  it('shows scenario-running restriction during replay, unlocks and notifies on handover, and blocks on ended', async () => {
    http.expectOne('api/app').flush(app);
    fake.last().open();
    fake.last().send(
      baseHello({
        seq: 1,
        rev: 1,
        phase: 'running',
        paused: false,
        paused_at_ms: null,
        holding: false,
        speed: 'max',
        pacing: 'on-time',
        stop: { requests: 0, by: null },
        scenario: true,
        handed_over: false,
        scenario_over_at_ms: null,
        allowed: { write: refusal('scenario-running', 'scenario plays') },
        outside: [],
      }),
    );
    fake.last().send({ type: 'synced', stream_seq: 2, run_id: '1', last_seq: 1 });
    await advance(BATCH_MS);
    TestBed.tick();
    for (const req of http.match((r) => r.url.startsWith('api/dashboard'))) {
      req.flush({
        file: 'simulation.yaml',
        path: 'simulation.dashboard.yaml',
        found: false,
        errors: [],
        dashboard: null,
      });
    }
    await advance(0);
    TestBed.tick();
    for (const block of await fixture.getDeferBlocks()) {
      await block.render(DeferBlockState.Complete);
    }
    TestBed.tick();

    expect(text('variables-notice')).toContain('シナリオの実行中は書けません（見るだけです）');
    const toggle = (fixture.nativeElement as HTMLElement).querySelector<HTMLButtonElement>(
      'button[role="switch"]',
    )!;
    expect(toggle.getAttribute('aria-disabled')).toBe('true');

    // Handover state arrives on the same run
    fake.last().send({
      type: 'event',
      stream_seq: 3,
      run_id: '1',
      event: {
        event: 'state',
        seq: 2,
        t_ms: 500,
        rev: 2,
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
        allowed: { write: ok, step: ok, resume: ok, pause: refusal('paused', 'paused') },
        outside: [],
      },
    });
    await advance(BATCH_MS);
    TestBed.tick();

    expect(text('variables-notice')).toContain('500 ms で完了し、手動操作のため一時停止しています');
    expect(toggle.getAttribute('aria-disabled')).not.toBe('true');
    toggle.click();
    const write = http.expectOne('api/write');
    expect(write.request.body).toEqual({
      run_id: '1',
      instance: 'inst-1',
      writes: { Engine: { 'engine.enabled': true } },
    });
    write.flush({ run_id: '1', operation: 1 });

    // Ended session never displays handover notice and blocks writes
    fake.last().send({
      type: 'variables',
      stream_seq: 4,
      run_id: '1',
      variables: {
        state: 'ended',
        reason: null,
        peers: [
          {
            name: 'Engine',
            connected: true,
            variables: [
              {
                name: 'engine.enabled',
                type: 'bool',
                role: 'input',
                writable: true,
                builtin: false,
                unit: '',
                min: null,
                max: null,
                step: null,
                description: '',
                enum: null,
              },
            ],
          },
        ],
        peers_omitted: 0,
        variables_omitted: 0,
        operations: [],
      },
    });
    await advance(BATCH_MS);
    TestBed.tick();
    expect(text('variables-notice')).toContain('実行は終わりました');
    expect(text('variables-notice')).not.toContain('手動操作のため一時停止');
    expect(toggle.getAttribute('aria-disabled')).toBe('true');
  });
});
