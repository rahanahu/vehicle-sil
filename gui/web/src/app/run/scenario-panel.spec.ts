import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { vi } from 'vitest';
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
    [rerunAllowed]="rerunAllowed()"
    (rerun)="reruns = reruns + 1"
  />`,
})
class Host {
  readonly scenario = signal<ScenarioView>(scenario(null));
  readonly running = signal(false);
  readonly paused = signal(false);
  readonly crashed = signal(false);
  readonly rerunAllowed = signal<boolean | null>(null);
  reruns = 0;
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

  it('renders template file:line provenance, step name, call_stack, case, and skipped state', async () => {
    fixture.componentInstance.scenario.set(
      scenario(null, {
        file: 'scenarios/cases.yaml',
        name: 'load cases',
        case: '定格',
        status: 'failed',
        steps: [
          {
            index: 1,
            file: 'scenarios/cases.yaml',
            line: 12,
            kind: 'set',
            name: null,
            text: 'set Engine.engine.throttle = 50',
            status: 'passed',
            start_ms: 0,
            at_ms: 0,
            end_ms: 0,
            code: null,
            message: null,
            call_stack: null,
          },
          {
            index: 2,
            file: 'scenarios/templates/check.yaml',
            line: 22,
            kind: 'expect',
            name: '定格: speed check',
            text: 'expect Engine.engine.speed ge 1700',
            status: 'failed',
            start_ms: 50,
            at_ms: 250,
            end_ms: 250,
            code: 'scenario-failed',
            message: 'too slow',
            call_stack: [
              {
                file: 'scenarios/cases.yaml',
                line: 31,
                name: 'run load check',
                params: { throttle: 50, label: '定格' },
              },
            ],
          },
          {
            index: 3,
            file: null,
            line: null,
            kind: 'expect',
            name: null,
            text: 'expect Engine.engine.turbo le 0.1',
            status: 'skipped',
            start_ms: null,
            at_ms: null,
            end_ms: null,
            code: 'scenario-skipped',
            message: 'absent variable',
            call_stack: null,
          },
        ],
      }),
    );
    await fixture.whenStable();

    const root = fixture.nativeElement as HTMLElement;
    expect(root.querySelector('[data-testid="scenario-case"]')?.textContent).toContain('定格');

    const rows = root.querySelectorAll<HTMLTableRowElement>('[data-testid="scenario-step"]');
    expect(rows.length).toBe(3);

    // Same file as root scenario -> line number only, no step name or call stack
    expect(rows[0]?.querySelector('.line')?.textContent?.trim()).toBe('12');
    expect(rows[0]?.querySelector('[data-testid="scenario-step-name"]')).toBeNull();
    expect(rows[0]?.querySelector('[data-testid="scenario-step-call-stack"]')).toBeNull();

    // Template file -> file:line, step name, and call_stack summary
    expect(rows[1]?.querySelector('.line')?.textContent?.trim()).toBe(
      'scenarios/templates/check.yaml:22',
    );
    expect(rows[1]?.querySelector('[data-testid="scenario-step-name"]')?.textContent).toBe(
      '定格: speed check',
    );
    const stackEl = rows[1]?.querySelector<HTMLElement>('[data-testid="scenario-step-call-stack"]');
    expect(stackEl?.textContent).toContain('scenarios/cases.yaml:31');
    expect(stackEl?.textContent).toContain('run load check');
    expect(stackEl?.textContent).toContain('throttle=50');

    // Null line -> em-dash, skipped state -> localized label
    expect(rows[2]?.querySelector('.line')?.textContent?.trim()).toBe('—');
    expect(rows[2]?.querySelector('.state')?.textContent).toContain('飛ばした');
  });

  it('renders 500+ steps with long provenance strings and skipped status cleanly', async () => {
    const longFile = `scenarios/templates/${'nested_'.repeat(20)}step.yaml`;
    const longName = `手順_${'長い名前_'.repeat(20)}`;
    const steps = Array.from({ length: 520 }, (_, i) => ({
      index: i + 1,
      file: i % 2 === 0 ? longFile : 'scenarios/large.yaml',
      line: i + 10,
      kind: 'set' as const,
      name: i === 0 ? longName : `step ${i + 1}`,
      text: `set Body.level = ${i % 100}`,
      status: i === 519 ? ('skipped' as const) : ('passed' as const),
      start_ms: i,
      at_ms: i,
      end_ms: i,
      code: i === 519 ? 'scenario-skipped' : null,
      message: i === 519 ? 'absent variable' : null,
      call_stack:
        i === 0
          ? [{ file: 'scenarios/large.yaml', line: 5, name: 'call', params: { p: 'x'.repeat(80) } }]
          : null,
    }));
    fixture.componentInstance.scenario.set(
      scenario(null, {
        file: 'scenarios/large.yaml',
        name: 'large',
        case: '長大ケース_' + 'c'.repeat(80),
        stepCount: 520,
        status: 'passed',
        steps,
      }),
    );
    await fixture.whenStable();

    const root = fixture.nativeElement as HTMLElement;
    const rows = root.querySelectorAll<HTMLTableRowElement>('[data-testid="scenario-step"]');
    expect(rows.length).toBe(520);
    expect(rows[0]?.querySelector('.line')?.textContent?.trim()).toBe(`${longFile}:10`);
    expect(rows[0]?.querySelector('[data-testid="scenario-step-name"]')?.textContent).toBe(
      longName,
    );
    expect(rows[519]?.querySelector('.state')?.textContent).toContain('飛ばした');
  });

  it('auto-expands timeline to failed step, jumps to clicked step, renders params, and emits rerun', async () => {
    fixture.componentInstance.rerunAllowed.set(true);
    fixture.componentInstance.scenario.set(
      scenario('api/timeline/9', {
        case: '定格',
        params: [
          { name: 'target_throttle', value: 50, from: 'case' },
          { name: 'min_speed', value: 2500, from: 'request_file' },
        ],
        status: 'failed',
        steps: [
          {
            index: 1,
            file: 'scenarios/cases.yaml',
            line: 10,
            kind: 'set',
            name: null,
            text: 'set Engine.engine.throttle = 50',
            status: 'passed',
            start_ms: 0,
            at_ms: 0,
            end_ms: 0,
            code: null,
            message: null,
            call_stack: null,
          },
          {
            index: 2,
            file: 'scenarios/templates/check.yaml',
            line: 18,
            kind: 'expect',
            name: 'speed tracking',
            text: 'expect Dashboard.dashboard.engine_speed near Engine.engine.speed',
            status: 'failed',
            start_ms: 50,
            at_ms: 150,
            end_ms: 150,
            code: 'scenario-failed',
            message: 'it was 1600 (150 ms) while Engine.engine.speed was 1800 (150 ms)',
            call_stack: [
              {
                file: 'scenarios/cases.yaml',
                line: 30,
                name: 'load check',
                params: { throttle: 50 },
              },
            ],
          },
        ],
      }),
    );
    await fixture.whenStable();

    const root = fixture.nativeElement as HTMLElement;
    expect(root.querySelector('[data-testid="scenario-params"]')?.textContent).toContain(
      'min_speed',
    );
    expect(root.querySelector('[data-testid="scenario-params"]')?.textContent).toContain('上書き');
    const toggle = root.querySelector<HTMLButtonElement>('[data-testid="timeline-toggle"]')!;
    expect(toggle.getAttribute('aria-expanded')).toBe('true');

    const details = root.querySelector<HTMLDetailsElement>(
      '[data-testid="scenario-step-call-stack"]',
    )!;
    expect(details.open).toBe(true);

    // Wait for the lazily imported <vehicle-sil-timeline> element to register and render
    await customElements.whenDefined('vehicle-sil-timeline');
    await vi.waitFor(async () => {
      await fixture.whenStable();
      expect(root.querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
        '選んだ手順: 2',
      );
    });

    // Jump button exists per step and focuses the clicked step
    const jump1 = root.querySelector<HTMLButtonElement>('[data-testid="scenario-step-jump-1"]')!;
    jump1.click();
    await fixture.whenStable();
    expect(root.querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
      '選んだ手順: 1',
    );

    // Rerun button emits rerun event
    root.querySelector<HTMLButtonElement>('[data-testid="scenario-rerun"]')?.click();
    expect(fixture.componentInstance.reruns).toBe(1);
  });
});
