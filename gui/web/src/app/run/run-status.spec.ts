import { Component, computed, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import {
  Control,
  FrontControls,
  RunOutcome,
  SessionSummary,
  SessionView,
  runOutcome,
} from '@vehicle-sil/gui-client';
import { sessionViewFrom } from '@vehicle-sil/gui-client/testing';
import { StopAction } from './run-session';
import { ACTION_GUARD, RunStatus } from './run-status';

/** 試験の中の、押せるようになってから押しても何もしない時間（本番より短く）。 */
const GUARD_MS = 200;

@Component({
  imports: [RunStatus],
  template: `
    <app-run-status
      [session]="session()"
      [outcome]="outcome()"
      [controls]="controls()"
      [ready]="ready()"
      [starting]="starting()"
      [stopping]="stopping()"
      [stopWhileStarting]="stopWhileStarting()"
      [elapsedMs]="null"
      [blockedBy]="[]"
      [otherFile]="otherFile()"
      (start)="starts = starts + 1"
      (stop)="stops = stops + 1"
    />
  `,
})
class Host {
  readonly session = signal<SessionView | null>(null);
  readonly ready = signal(true);
  readonly starting = signal(false);
  readonly stopping = signal<StopAction | null>(null);
  readonly stopWhileStarting = signal(false);
  readonly otherFile = signal(false);
  starts = 0;
  stops = 0;
  /** 正面の口の代わり: 持ち主の言う可否を、この試験の状態から組み立てる（理由の code は持ち主のもの）。 */
  readonly controls = computed<FrontControls>(() => {
    const session = this.session();
    const running = session?.launcher.running ?? false;
    const asked = (session?.stopRequests ?? 0) > 0 || this.stopping() !== null;
    const no = (code: string): Control => ({
      ok: false,
      reason: { code, message: code },
      caution: null,
    });
    const yes: Control = { ok: true, reason: null, caution: null };
    const idle = no('not-running');
    return {
      start: !this.ready()
        ? no('connecting')
        : running
          ? no('already-running')
          : this.starting()
            ? no('busy')
            : yes,
      stop: !(running || this.starting())
        ? no('not-running')
        : asked || this.stopWhileStarting()
          ? no('already-stopping')
          : yes,
      force_stop: running && this.ready() && this.stopping() === null ? yes : idle,
      pause: idle,
      resume: idle,
      step: { ...idle, min_ms: 1, max_ms: 1000 },
      run_until: idle,
      speed: { ...idle, min: 0.1, max: 10 },
    };
  });
  outcome(): RunOutcome | null {
    const session = this.session();
    return session ? runOutcome(session) : null;
  }
}

function summary(extra: Partial<SessionSummary> = {}): SessionSummary {
  return {
    id: 1,
    file: 'sim.yaml',
    disable: [],
    manual: [],
    speed: null,
    start_paused: false,
    state: 'running',
    phase: 'running',
    stop: 'none',
    last_seq: 3,
    last_t_ms: null,
    sim_ms: null,
    time_omitted_fields: [],
    time: null,
    last_command: null,
    check: null,
    participants: [],
    findings: [],
    result: null,
    launcher: { running: true, exit_code: null, signal: null, silent_s: null },
    log: {},
    problems: [],
    problems_omitted: 0,
    scenario: null,
    unreadable: [],
    ...extra,
  };
}

const ENDED = {
  state: 'ended',
  launcher: { running: false, exit_code: 0, signal: null, silent_s: null },
  result: {
    result: 'passed',
    findings: [],
    participants: [],
  } as unknown as SessionSummary['result'],
} satisfies Partial<SessionSummary>;

const pause = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

describe('RunStatus', () => {
  let fixture: ComponentFixture<Host>;
  let host: Host;

  beforeEach(async () => {
    TestBed.configureTestingModule({ providers: [{ provide: ACTION_GUARD, useValue: GUARD_MS }] });
    fixture = TestBed.createComponent(Host);
    host = fixture.componentInstance;
    await settle();
  });

  function query(testId: string): HTMLElement | null {
    return (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="${testId}"]`);
  }

  const startButton = () => query('start') as HTMLButtonElement;
  const stopButton = () => query('stop') as HTMLButtonElement;

  /** 変わった後、押せるようになったボタンが押せるまで待つ。 */
  async function settle(): Promise<void> {
    await fixture.whenStable();
    await pause(GUARD_MS + 50);
    await fixture.whenStable();
  }

  async function show(session: SessionView | null): Promise<void> {
    host.session.set(session);
    await settle();
  }

  it('keeps start and stop as two buttons in fixed places, and only one of them can be pressed', async () => {
    const [start, stop] = [startButton(), stopButton()];
    const order = () =>
      [...(fixture.nativeElement as HTMLElement).querySelectorAll('.actions button')].slice(0, 2);
    expect(order()).toEqual([start, stop]);
    expect(start.textContent).toContain('開始');
    expect(start.getAttribute('aria-disabled')).not.toBe('true');
    expect(stop.getAttribute('aria-disabled')).toBe('true');

    await show(sessionViewFrom(summary()));
    expect(order()).toEqual([start, stop]);
    expect(start.getAttribute('aria-disabled')).toBe('true');
    expect(stop.getAttribute('aria-disabled')).not.toBe('true');
    start.click();
    expect(host.starts).toBe(0);
    stop.click();
    expect(host.stops).toBe(1);
  });

  it('ignores a press right after a button became pressable (a double click, a repeated Enter)', async () => {
    host.session.set(sessionViewFrom(summary()));
    await fixture.whenStable();
    // 停止は押せるようになったばかり: 薄く出て、押しても何もしない
    expect(stopButton().getAttribute('aria-disabled')).not.toBe('true');
    expect(stopButton().getAttribute('data-armed')).toBe('false');
    stopButton().click();
    expect(host.stops).toBe(0);
    await pause(GUARD_MS + 50);
    await fixture.whenStable();
    expect(stopButton().getAttribute('data-armed')).toBe('true');
    stopButton().click();
    expect(host.stops).toBe(1);
  });

  it('does not start a run with a click meant for stop just as the run ends', async () => {
    const running = sessionViewFrom(summary());
    await show(running);
    host.session.set(sessionViewFrom(summary(), summary(ENDED)));
    await fixture.whenStable();
    // 利用者は同じ位置（停止）を押す: 停止は押せない。開始は別の位置にあり、押せるようになったばかり
    stopButton().click();
    startButton().click();
    expect(host.stops).toBe(0);
    expect(host.starts).toBe(0);
    expect(startButton().textContent).toContain('もう一度実行');
    await pause(GUARD_MS + 50);
    await fixture.whenStable();
    startButton().click();
    expect(host.starts).toBe(1);
  });

  it('does not stop without waiting on a second click', async () => {
    const running = sessionViewFrom(summary());
    await show(running);
    stopButton().click();
    host.stopping.set('stop');
    await fixture.whenStable();
    stopButton().click();
    // 停止を受け付けた要約が当たった後の 2 回目のクリック
    host.stopping.set(null);
    await show(sessionViewFrom(summary(), summary({ stop: 'requested' })));
    stopButton().click();
    expect(host.stops).toBe(1);
    expect(stopButton().textContent).toContain('止めています');
    expect(stopButton().getAttribute('aria-disabled')).toBe('true');
    expect(query('force-stop')).not.toBeNull();
  });

  it('does not say that it stops once the stopped run has ended', async () => {
    await show(sessionViewFrom(summary({ stop: 'requested' })));
    expect(stopButton().textContent).toContain('止めています');
    await show(
      sessionViewFrom(
        summary({
          ...ENDED,
          stop: 'requested',
          launcher: { running: false, exit_code: 130, signal: null, silent_s: null },
        }),
      ),
    );
    expect(stopButton().getAttribute('aria-disabled')).toBe('true');
    expect(stopButton().textContent).not.toContain('止めています');
    expect(stopButton().textContent).toContain('停止');
  });

  it('says when the simulation is paused, its speed, and how late it is, in words', async () => {
    // 一時停止しているかと速さは、ランチャーの state が言う（time の事象には無い）
    const state = (paused: boolean, speed: number | 'max' = 1) =>
      ({
        seq: 3,
        rev: 3,
        phase: 'running',
        paused,
        paused_at_ms: paused ? 0 : null,
        holding: false,
        speed,
        pacing: null,
        stop: null,
        scenario: false,
        allowed: {},
      }) satisfies SessionSummary['launcher_state'];
    const time = (extra: Partial<NonNullable<SessionSummary['time']>> = {}) => ({
      sim_ms: 0,
      actual_speed: null,
      late_steps: 0,
      max_late_ms: 0,
      slipped_ms: 0,
      late_ms: 0,
      ...extra,
    });
    await show(sessionViewFrom(summary({ sim_ms: 0, time: time(), launcher_state: state(true) })));
    expect(query('outcome')?.textContent).toContain('一時停止中');
    expect(query('speed')?.textContent).toContain('等速');
    expect(query('sim-time')?.textContent).toContain('0.000');
    expect(query('late')?.textContent?.trim()).toBe('');
    // 読み上げるのは、遅れが「ある」になったときの決まった文だけ（数は読み上げない）
    expect(query('late-announce')?.getAttribute('role')).toBe('status');
    expect(query('late')?.getAttribute('role')).toBeNull();
    // 許容の内の小さな遅れ（刻みの同期のたびに出たり消えたりする）は、遅れとして言わない
    await show(
      sessionViewFrom(
        summary({
          sim_ms: 100,
          time: time({ sim_ms: 100, late_ms: 3 }),
          launcher_state: state(false),
        }),
      ),
    );
    expect(query('late')?.textContent?.trim()).toBe('');
    expect(query('late-announce')?.textContent?.trim()).toBe('');
    await show(
      sessionViewFrom(
        summary({
          sim_ms: 300,
          time: time({ sim_ms: 300, late_steps: 1, max_late_ms: 200, slipped_ms: 200 }),
          launcher_state: state(false),
        }),
      ),
    );
    expect(query('outcome')?.textContent).toContain('実行中');
    expect(query('late')?.textContent).toContain('実時間に遅れました');
    expect(query('late')?.textContent).toContain('200 ms');
    expect(query('late-announce')?.textContent?.trim()).toBe('実時間に遅れました。');
    // 数が変わっても、読み上げる文は変わらない
    await show(
      sessionViewFrom(
        summary({
          sim_ms: 900,
          time: time({ sim_ms: 900, late_steps: 2, max_late_ms: 300, slipped_ms: 500 }),
          launcher_state: state(false),
        }),
      ),
    );
    expect(query('late')?.textContent).toContain('500 ms');
    expect(query('late-announce')?.textContent?.trim()).toBe('実時間に遅れました。');
    // 途中で最速に変えても、それまでの遅れは出したまま
    await show(
      sessionViewFrom(
        summary({
          sim_ms: 300,
          time: time({ late_steps: 1 }),
          launcher_state: state(false, 'max'),
        }),
      ),
    );
    expect(query('late')?.textContent).toContain('実時間に遅れました');
    expect(query('speed')?.textContent).toContain('最速');
  });

  it('says only what it waits for while stopping (the button says that it stops)', async () => {
    await show(sessionViewFrom(summary({ stop: 'requested' })));
    expect(stopButton().textContent).toContain('止めています');
    expect(query('outcome')?.textContent).toContain('participant が終わるのを待っています');
    expect(query('outcome')?.textContent).not.toContain('止めています');
    await show(sessionViewFrom(summary({ stop: 'forced' })));
    expect(query('outcome')?.textContent).toContain('強制終了するのを待っています');
  });

  it('can be asked to stop while the start waits for its answer', async () => {
    host.starting.set(true);
    await settle();
    expect(query('outcome')?.textContent).toContain('始めています');
    expect(startButton().getAttribute('aria-disabled')).toBe('true');
    stopButton().click();
    expect(host.stops).toBe(1);
    host.stopWhileStarting.set(true);
    await settle();
    expect(query('outcome')?.textContent).toContain('開始をやめます');
    expect(stopButton().textContent).toContain('止めています');
  });

  it('can stop a running launcher while the WebSocket is down, and says what it does not see', async () => {
    host.ready.set(false);
    await show(sessionViewFrom(summary()));
    expect(query('blocked')?.textContent).toContain('最新の状態は見えていませんが');
    stopButton().click();
    expect(host.stops).toBe(1);
    // 開始は、揃うまでできない
    await show(sessionViewFrom(summary(ENDED)));
    expect(startButton().getAttribute('aria-disabled')).toBe('true');
    expect(query('blocked')?.textContent).toContain('サーバーと揃うまで');
  });

  it('shows a run the user stopped in a neutral tone, not as a failure', async () => {
    const result = {
      result: 'interrupted',
      findings: [{ severity: 'note', code: 'interrupted', message: 'm' }],
      participants: [],
    };
    await show(
      sessionViewFrom(
        summary({
          state: 'ended',
          launcher: { running: false, exit_code: 130, signal: null, silent_s: null },
          result: result as unknown as SessionSummary['result'],
        }),
      ),
    );
    expect(query('outcome')?.getAttribute('data-tone')).toBe('neutral');
    expect(query('outcome')?.textContent).toContain('■');
    expect(query('outcome')?.textContent).not.toContain('✕');
    expect(query('outcome')?.textContent).toContain('中断');
    expect(query('first-error')).toBeNull();
  });

  it('says when the launcher has written nothing for a while, in a status region that is always there', async () => {
    expect(query('silent')?.getAttribute('role')).toBe('status');
    await show(
      sessionViewFrom(
        summary({ launcher: { running: true, exit_code: null, signal: null, silent_s: 10 } }),
      ),
    );
    expect(query('silent')?.textContent).toContain('10 秒以上、何も届いていません');
    await show(sessionViewFrom(summary()));
    expect(query('silent')?.textContent?.trim()).toBe('');
  });

  it('names the parts it cannot show', async () => {
    await show(
      sessionViewFrom(summary({ unreadable: ['result.participants', 'result.findings'] })),
    );
    expect(query('unreadable')?.textContent).toContain('一部を表示できません');
    expect(query('unreadable')?.textContent).toContain('result.participants');
  });

  it('can stop a launcher that runs on after its result', async () => {
    const result = { result: 'passed', findings: [], participants: [] };
    await show(sessionViewFrom(summary({ result: result as unknown as SessionSummary['result'] })));
    expect(stopButton().textContent).toContain('停止');
    expect(stopButton().getAttribute('aria-disabled')).not.toBe('true');
    expect(query('outcome')?.textContent).toContain('ランチャーが終わるのを待っています');
  });

  it('says that the launcher ended without a result, and moves the phases to the end', async () => {
    await show(
      sessionViewFrom(
        summary({
          state: 'ended',
          launcher: { running: false, exit_code: 70, signal: null, silent_s: null },
        }),
      ),
    );
    expect(query('outcome')?.textContent).toContain('異常終了（終了コード 70）');
    expect(query('outcome')?.getAttribute('role')).toBe('status');
    const phases = (fixture.nativeElement as HTMLElement).querySelector('[aria-label="段階"]');
    expect(phases?.querySelector('[aria-current="step"]')?.textContent).toContain('終了');
    expect(phases?.textContent).toContain('実行（中断）');
    expect(phases?.querySelector('.todo')?.textContent).toContain('停止中');
    expect(startButton().textContent).toContain('もう一度実行');
  });

  it('starts, not runs again, when another file is chosen, and says the result is of the run before', async () => {
    await show(
      sessionViewFrom(
        summary({
          state: 'ended',
          launcher: { running: false, exit_code: 70, signal: null, silent_s: null },
        }),
      ),
    );
    host.otherFile.set(true);
    await settle();
    expect(startButton().textContent).toContain('開始');
    expect(startButton().textContent).not.toContain('もう一度');
    expect(query('outcome')?.textContent).toContain('前の実行: 異常終了（終了コード 70）');
  });

  it('does not start until it is in step with the server', async () => {
    host.ready.set(false);
    await settle();
    startButton().click();
    expect(host.starts).toBe(0);
    expect(query('blocked')?.textContent).toContain('サーバーと揃うまで');
  });
});
