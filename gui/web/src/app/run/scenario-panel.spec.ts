import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { ScenarioView } from '@vehicle-sil/gui-client';
import { ScenarioPanel, timelinePageUrl } from './scenario-panel';

function scenario(timelineUrl: string | null): ScenarioView {
  return {
    file: 'scenarios/fails.yaml',
    name: 'fails',
    stepCount: 3,
    steps: [],
    stepsOmitted: 0,
    status: timelineUrl ? 'failed' : null,
    overAtMs: null,
    timelineUrl,
  };
}

@Component({
  imports: [ScenarioPanel],
  template: `<app-scenario-panel [scenario]="scenario()" />`,
})
class Host {
  readonly scenario = signal<ScenarioView>(scenario(null));
}

describe('ScenarioPanel', () => {
  let fixture: ComponentFixture<Host>;
  const link = () =>
    (fixture.nativeElement as HTMLElement).querySelector<HTMLAnchorElement>(
      '[data-testid="scenario-timeline"]',
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
});
