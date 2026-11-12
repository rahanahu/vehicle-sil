import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { SessionSummary } from '../core/api';
import {
  RunOutcome,
  SessionView,
  applySummary,
  runOutcome,
  sessionFromSummary,
} from '../core/session/session-state';
import { RunAction } from './run-session';
import { RunStatus } from './run-status';

@Component({
  imports: [RunStatus],
  template: `
    <app-run-status
      [session]="session()"
      [outcome]="outcome()"
      [ready]="ready()"
      [pending]="pending()"
      [elapsedMs]="null"
      [blockedBy]="[]"
      (start)="starts = starts + 1"
      (stop)="stops = stops + 1"
    />
  `,
})
class Host {
  readonly session = signal<SessionView | null>(null);
  readonly ready = signal(true);
  readonly pending = signal<RunAction | null>(null);
  starts = 0;
  stops = 0;
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
    speed: null,
    state: 'running',
    phase: 'running',
    stop: 'none',
    last_seq: 3,
    last_t_ms: null,
    sim_ms: null,
    time_omitted_fields: [],
    check: null,
    participants: [],
    findings: [],
    result: null,
    launcher: { running: true, exit_code: null, signal: null },
    log: {},
    problems: [],
    problems_omitted: 0,
    ...extra,
  };
}

describe('RunStatus', () => {
  let fixture: ComponentFixture<Host>;

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  function query(testId: string): HTMLElement | null {
    return (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="${testId}"]`);
  }

  async function show(session: SessionView | null): Promise<void> {
    fixture.componentInstance.session.set(session);
    await fixture.whenStable();
  }

  it('keeps the focus on the one button that starts and stops', async () => {
    const button = query('primary') as HTMLButtonElement;
    expect(button.textContent).toContain('開始');
    button.focus();
    await show(sessionFromSummary(summary()));
    expect(query('primary')).toBe(button);
    expect(document.activeElement).toBe(button);
    expect(button.textContent).toContain('停止');
    button.click();
    expect(fixture.componentInstance.stops).toBe(1);
  });

  it('does not stop without waiting on a second click', async () => {
    const running = sessionFromSummary(summary());
    await show(running);
    const button = query('primary') as HTMLButtonElement;
    button.click();
    // 停止を受け付けた要約が当たった後の 2 回目のクリック
    await show(applySummary(running, summary({ stop: 'requested' })));
    button.click();
    expect(fixture.componentInstance.stops).toBe(1);
    expect(button.textContent).toContain('止めています');
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(query('force-stop')).not.toBeNull();
  });

  it('can stop a launcher that runs on after its result', async () => {
    const result = { result: 'passed', findings: [], participants: [] };
    await show(
      sessionFromSummary(summary({ result: result as unknown as SessionSummary['result'] })),
    );
    expect(query('primary')?.textContent).toContain('停止');
    expect(query('outcome')?.textContent).toContain('ランチャーが終わるのを待っています');
  });

  it('says that the launcher ended without a result, and moves the phases to the end', async () => {
    await show(
      sessionFromSummary(
        summary({ state: 'ended', launcher: { running: false, exit_code: 70, signal: null } }),
      ),
    );
    expect(query('outcome')?.textContent).toContain('異常終了（終了コード 70）');
    expect(query('outcome')?.getAttribute('role')).toBe('status');
    const phases = (fixture.nativeElement as HTMLElement).querySelector('[aria-label="段階"]');
    expect(phases?.querySelector('[aria-current="step"]')?.textContent).toContain('終了');
    expect(phases?.textContent).toContain('実行（中断）');
    expect(phases?.querySelector('.todo')?.textContent).toContain('停止中');
    expect(query('primary')?.textContent).toContain('もう一度実行');
  });

  it('does not start or stop until it is in step with the server', async () => {
    fixture.componentInstance.ready.set(false);
    await show(sessionFromSummary(summary()));
    const button = query('primary') as HTMLButtonElement;
    button.click();
    expect(fixture.componentInstance.stops).toBe(0);
    expect(query('blocked')?.textContent).toContain('サーバーと揃うまで');
  });
});
