import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { ScenarioView } from '@vehicle-sil/gui-client';
import { ScenarioPanel, timelinePageUrl } from './scenario-panel';

function scenario(timelineUrl: string | null, extra: Partial<ScenarioView> = {}): ScenarioView {
  return {
    file: 'scenarios/fails.yaml',
    name: 'fails',
    stepCount: 3,
    steps: [],
    stepsOmitted: 0,
    status: timelineUrl ? 'failed' : null,
    handedOver: false,
    overAtMs: null,
    timelineUrl,
    ...extra,
  };
}

@Component({
  imports: [ScenarioPanel],
  template: `<app-scenario-panel
    [scenario]="scenario()"
    [running]="running()"
    [paused]="paused()"
    [crashed]="crashed()"
  />`,
})
class Host {
  readonly scenario = signal<ScenarioView>(scenario(null));
  readonly running = signal(false);
  readonly paused = signal(false);
  readonly crashed = signal(false);
}

describe('ScenarioPanel', () => {
  let fixture: ComponentFixture<Host>;
  const link = () =>
    (fixture.nativeElement as HTMLElement).querySelector<HTMLAnchorElement>(
      '[data-testid="scenario-timeline"]',
    );
  const handover = () =>
    (fixture.nativeElement as HTMLElement).querySelector<HTMLElement>(
      '[data-testid="scenario-handover"]',
    );

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  it('links the timeline page once the run has left a timeline file, in a new tab with the opener', async () => {
    expect(link()).toBeNull();
    fixture.componentInstance.scenario.set(scenario('api/timeline/3'));
    await fixture.whenStable();
    const a = link();
    expect(a?.getAttribute('href')).toBe('timeline/?src=%2Fapi%2Ftimeline%2F3&lang=ja');
    expect(a?.target).toBe('_blank');
    // The new tab gets a copy of this tab's sessionStorage (the token) only with an opener
    expect(a?.rel).toBe('opener');
  });

  it('puts no token in the address of the page', () => {
    expect(timelinePageUrl('api/timeline/12', 'en')).toBe(
      'timeline/?src=%2Fapi%2Ftimeline%2F12&lang=en',
    );
  });

  it('shows the handover badge only for an active non-crashed passed handover run', async () => {
    expect(handover()).toBeNull();

    fixture.componentInstance.running.set(true);
    fixture.componentInstance.paused.set(true);
    fixture.componentInstance.scenario.set(
      scenario(null, { status: 'passed', handedOver: true, overAtMs: 500 }),
    );
    await fixture.whenStable();
    expect(handover()?.textContent).toContain('500 ms');
    expect(handover()?.textContent).toContain('一時停止しています');

    // Resumed after handover
    fixture.componentInstance.paused.set(false);
    await fixture.whenStable();
    expect(handover()?.textContent).toContain('500 ms');
    expect(handover()?.textContent).toContain('引き継ぎました');

    // Hidden when crashed, ended (running=false), or failed/incomplete/non-handedOver
    fixture.componentInstance.crashed.set(true);
    await fixture.whenStable();
    expect(handover()).toBeNull();

    fixture.componentInstance.crashed.set(false);
    fixture.componentInstance.running.set(false);
    await fixture.whenStable();
    expect(handover()).toBeNull();

    fixture.componentInstance.running.set(true);
    for (const bad of [
      { status: 'failed' as const, handedOver: true },
      { status: 'incomplete' as const, handedOver: true },
      { status: 'passed' as const, handedOver: false },
    ]) {
      fixture.componentInstance.scenario.set(scenario(null, { ...bad, overAtMs: 500 }));
      await fixture.whenStable();
      expect(handover()).toBeNull();
    }
  });
});
