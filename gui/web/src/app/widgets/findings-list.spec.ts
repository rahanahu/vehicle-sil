import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Finding } from '@vehicle-sil/gui-client';
import { FindingsList } from './findings-list';

function finding(severity: string, code: string): Finding {
  return {
    severity,
    code,
    file: 'simulation.yaml',
    line: null,
    participant: null,
    network: null,
    message: `${code}: what is wrong and how to fix it`,
    detail: null,
  };
}

@Component({
  imports: [FindingsList],
  template: `<app-findings-list [findings]="findings()" />`,
})
class Host {
  readonly findings = signal<readonly Finding[]>([]);
}

describe('FindingsList', () => {
  let fixture: ComponentFixture<Host>;

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  it('shows a code it does not know by its severity, and an unknown severity as an error', async () => {
    // 画面は code ごとの決まりを持たない: ランチャーに新しい所見が増えても、重さで並べて、そのまま出す
    fixture.componentInstance.findings.set([
      finding('note', 'registry-on-network'),
      finding('warning', 'connection-not-closed'),
      finding('severe', 'from-a-newer-launcher'),
      finding('error', 'describe-unknown-value'),
    ]);
    await fixture.whenStable();
    const items = [
      ...(fixture.nativeElement as HTMLElement).querySelectorAll('[data-testid="finding"]'),
    ];
    expect(
      items.map((item) => [
        item.getAttribute('data-severity'),
        item.querySelector('.code')?.textContent,
        item.querySelector('.severity')?.textContent?.trim(),
      ]),
    ).toEqual([
      ['0', 'from-a-newer-launcher', '✕ 誤り'],
      ['0', 'describe-unknown-value', '✕ 誤り'],
      ['1', 'connection-not-closed', '! 警告'],
      ['2', 'registry-on-network', 'i 注記'],
    ]);
    expect(items[2].querySelector('.message')?.textContent).toBe(
      'connection-not-closed: what is wrong and how to fix it',
    );
  });
});
