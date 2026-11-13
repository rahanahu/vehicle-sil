import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { ParticipantView } from '@vehicle-sil/gui-client';
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
    controlLink: null,
    manual: false,
    commandLine: null,
    workingDirectory: null,
    ...extra,
  };
}

function link(
  state: string,
  extra: { variables?: number; version?: number } = {},
): ParticipantView['controlLink'] {
  return { state, variables: null, version: null, late_messages: null, ...extra };
}

@Component({
  imports: [ParticipantStatus],
  template: `<app-participant-status
    [participants]="participants()"
    [launcherGone]="gone()"
    [ended]="ended()"
    [stays]="stays()"
  />`,
})
class Host {
  readonly participants = signal<readonly ParticipantView[]>([]);
  readonly gone = signal(false);
  readonly ended = signal(false);
  readonly stays = signal<ReadonlyMap<string, string>>(new Map());
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

  it('shows how each participant is linked to its variables, a trouble in words', async () => {
    fixture.componentInstance.participants.set([
      participant('Engine', { controlLink: link('connected', { variables: 17 }) }),
      participant('Dashboard', {
        controlLink: link('version_mismatch', { version: 2 }),
      }),
      participant('Gauge', { controlLink: link('no_hello') }),
      participant('Meter', { controlLink: link('not_offered') }),
      participant('Logger', { controlLink: link('none') }),
      participant('Old'),
    ]);
    await fixture.whenStable();
    const control = (name: string) =>
      (fixture.nativeElement as HTMLElement).querySelector(`[data-testid="control-${name}"]`);
    expect(control('Engine')?.textContent).toContain('つながった（17 個）');
    expect(control('Dashboard')?.textContent).toContain('形式の版 2 が違う');
    expect(control('Dashboard')?.textContent).toContain('⚠');
    expect(control('Gauge')?.textContent).toContain('hello が来なかった');
    expect(control('Meter')?.textContent).toContain('読み書きの口を出していない');
    expect(control('Meter')?.textContent).not.toContain('⚠');
    expect(control('Logger')?.textContent).toContain('変数: なし');
    expect(control('Old')).toBeNull(); // 口を知らせない古いランチャー
  });

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

  it('shows the command to start a participant the user starts, as text, until it joins', async () => {
    const line = '/ecu/dash --name Dashboard <b>x</b>';
    const manual = { manual: true, commandLine: line, workingDirectory: '/root dir' };
    fixture.componentInstance.participants.set([
      participant('Dashboard', { state: 'manual', pid: null, ...manual }),
    ]);
    await fixture.whenStable();
    expect(row('Dashboard')?.textContent).toContain('自分で起動する: 参加を待っています');
    const command = row('Dashboard')?.querySelector('[data-testid="manual-command-Dashboard"]');
    expect(command?.querySelector('pre')?.textContent).toBe(line); // HTML として読まない
    expect(command?.querySelector('b')).toBeNull();
    expect(command?.textContent).toContain('/root dir');

    fixture.componentInstance.participants.set([
      participant('Dashboard', { state: 'joined', pid: null, ...manual }),
    ]);
    await fixture.whenStable();
    expect(row('Dashboard')?.textContent).toContain('参加した（自分で起動）');
    expect(row('Dashboard')?.querySelector('[data-testid="manual-command-Dashboard"]')).toBeNull();

    // 終わった後も残っているとランチャーが知らせた
    fixture.componentInstance.ended.set(true);
    fixture.componentInstance.stays.set(
      new Map([['Dashboard', 'Dashboard (started by you, manual) ...']]),
    );
    await fixture.whenStable();
    expect(
      row('Dashboard')?.querySelector('[data-testid="manual-stays-Dashboard"]')?.textContent,
    ).toContain('自分で終わらせてください');

    // 参加しないまま終わった
    fixture.componentInstance.participants.set([
      participant('Dashboard', { state: 'manual', pid: null, ...manual }),
    ]);
    await fixture.whenStable();
    expect(row('Dashboard')?.textContent).toContain('参加しなかった（自分で起動）');
    expect(row('Dashboard')?.querySelector('[data-testid="manual-command-Dashboard"]')).toBeNull();
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
