import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { ParticipantView } from '../core/session/session-state';
import { ParticipantStatus } from './participant-status';

function participant(name: string, extra: Partial<ParticipantView> = {}): ParticipantView {
  return {
    name,
    state: 'joined',
    required: true,
    pid: 1,
    exitCode: null,
    signal: null,
    failure: null,
    message: null,
    stopRequested: false,
    omittedFields: [],
    ...extra,
  };
}

@Component({
  imports: [ParticipantStatus],
  template: `<app-participant-status [participants]="participants()" [launcherGone]="gone()" />`,
})
class Host {
  readonly participants = signal<readonly ParticipantView[]>([]);
  readonly gone = signal(false);
}

describe('ParticipantStatus', () => {
  let fixture: ComponentFixture<Host>;

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  function row(name: string): HTMLElement | null {
    return (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="status-${name}"]`);
  }

  it('shows a participant whose end was never told as unknown once the launcher is gone', async () => {
    fixture.componentInstance.participants.set([
      participant('Engine'),
      participant('Dashboard', { state: 'ended', exitCode: 0 }),
      participant('Logger', { state: 'disabled', pid: null }),
      participant('Late', { state: null, pid: null }),
    ]);
    await fixture.whenStable();
    expect(row('Engine')?.textContent).toContain('参加した');

    fixture.componentInstance.gone.set(true);
    await fixture.whenStable();
    expect(row('Engine')?.textContent).toContain('不明');
    expect(
      (fixture.nativeElement as HTMLElement).querySelector('[data-testid="participants-unknown"]')
        ?.textContent,
    ).toContain('ランチャーが先に終わった');
    expect(row('Engine')?.textContent).toContain('最後の知らせ: 参加した');
    expect(row('Engine')?.getAttribute('data-state')).toBe('unknown');
    expect(row('Late')?.textContent).toContain('不明');
    expect(row('Late')?.textContent).not.toContain('最後の知らせ');
    // 終わりが届いたものは、そのまま
    expect(row('Dashboard')?.textContent).toContain('終了した');
    expect(row('Dashboard')?.textContent).toContain('終了コード 0');
    expect(row('Logger')?.textContent).toContain('外した');
  });

  it('shows a participant the launcher has decided failed at once, with the reason in words', async () => {
    fixture.componentInstance.participants.set([
      participant('Engine', { state: 'failed', failure: 'left_simulation', message: 'it left' }),
    ]);
    await fixture.whenStable();
    expect(row('Engine')?.textContent).toContain('失敗');
    expect(row('Engine')?.textContent).toContain('シミュレーションを去った');
    expect(row('Engine')?.getAttribute('data-state')).toBe('bad');
    // ランチャーの英語の値は title に
    expect(row('Engine')?.textContent).not.toContain('left_simulation');
    expect(row('Engine')?.querySelector('[title="left_simulation: it left"]')).not.toBeNull();
    // プロセスが終わっても失敗のまま
    fixture.componentInstance.participants.set([
      participant('Engine', { state: 'ended', exitCode: 0, failure: 'left_simulation' }),
    ]);
    await fixture.whenStable();
    expect(row('Engine')?.textContent).toContain('失敗');
    expect(row('Engine')?.getAttribute('data-state')).toBe('bad');
  });

  it('does not show a participant the launcher stopped as failed', async () => {
    fixture.componentInstance.participants.set([
      participant('Engine', { state: 'ended', exitCode: 130, stopRequested: true }),
      participant('Logger', {
        state: 'ended',
        exitCode: null,
        signal: 'SIGTERM',
        stopRequested: true,
      }),
    ]);
    await fixture.whenStable();
    expect(row('Engine')?.getAttribute('data-state')).toBe('stopped');
    expect(row('Logger')?.getAttribute('data-state')).toBe('stopped');
    expect(row('Engine')?.textContent).toContain('■');
  });

  it('names the fields the server left out', async () => {
    fixture.componentInstance.participants.set([
      participant('Engine', { omittedFields: ['command', 'environment'] }),
      participant('Dashboard'),
    ]);
    await fixture.whenStable();
    expect(row('Engine')?.textContent).toContain('大きすぎて省いた欄: command');
    expect(row('Engine')?.textContent).toContain('environment');
    expect(row('Dashboard')?.textContent).not.toContain('省いた');
  });
});
