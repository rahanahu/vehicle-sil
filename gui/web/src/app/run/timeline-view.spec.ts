import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { vi } from 'vitest';
import type {
  VehicleSilTimelineElement,
  VehicleSilTimelineLoadDetail,
} from '@vehicle-sil/gui-client/timeline';
import { TimelineView } from './timeline-view';

function makeLoadDetail(
  overrides: Partial<VehicleSilTimelineLoadDetail> = {},
): VehicleSilTimelineLoadDetail {
  return {
    file: 'aeb.timeline.json',
    scenario: 'AEB emergency braking',
    status: 'failed',
    start: 0,
    end: 977.6,
    recordedTo: 640,
    stepCount: 10,
    failure: {
      step: 10,
      at: 640,
      name: 'braking pressure tracking',
      text: 'ramp Brake.hyd.target_pressure to 120',
      message: 'Brake.hyd.pressure near target_pressure',
      conditions: [
        {
          variable: 'Brake.hyd.pressure',
          compare: 'near Brake.hyd.target_pressure ± 15',
          valueText: '88',
          sampleMs: 640,
          compared: [{ variable: 'Brake.hyd.target_pressure', valueText: '118', sampleMs: 640 }],
        },
      ],
      writes: [],
    },
    variables: [
      {
        name: 'ADAS.aeb.active',
        participant: 'ADAS',
        type: 'bool',
        causality: 'input',
        unit: '',
        written: true,
        watched: true,
        recorded: false,
        failing: false,
        relatedToFailure: false,
        changedNearFailure: false,
      },
      {
        name: 'ADAS.aeb.state',
        participant: 'ADAS',
        type: 'enum',
        causality: 'output',
        unit: '',
        written: false,
        watched: true,
        recorded: false,
        failing: false,
        relatedToFailure: false,
        changedNearFailure: true,
      },
      {
        name: 'Brake.hyd.pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'output',
        unit: 'bar',
        written: false,
        watched: true,
        recorded: false,
        failing: true,
        relatedToFailure: true,
        changedNearFailure: true,
      },
      {
        name: 'Brake.hyd.target_pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'input',
        unit: 'bar',
        written: true,
        watched: true,
        recorded: false,
        failing: true,
        relatedToFailure: true,
        changedNearFailure: true,
      },
    ],
    ...overrides,
  };
}

@Component({
  imports: [TimelineView],
  template: `<app-timeline-view
    [timelineUrl]="url()"
    [autoExpand]="autoExpand()"
    [initialStep]="initialStep()"
  />`,
})
class Host {
  readonly url = signal('api/timeline/1');
  readonly autoExpand = signal(true);
  readonly initialStep = signal<number | null>(10);
}

describe('TimelineView', () => {
  let fixture: ComponentFixture<Host>;
  const root = () => fixture.nativeElement as HTMLElement;
  const timelineEl = () =>
    root().querySelector<VehicleSilTimelineElement>('[data-testid="timeline-element"]');

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await customElements.whenDefined('vehicle-sil-timeline');
    await vi.waitFor(async () => {
      await fixture.whenStable();
      expect(timelineEl()).not.toBeNull();
    });
  });

  function emitLoad(detail: VehicleSilTimelineLoadDetail): void {
    const el = timelineEl()!;
    el.dispatchEvent(new CustomEvent('load', { detail }));
    fixture.detectChanges();
  }

  it('filters by search, presets, participants, and inline hide/restore without resetting cursor or step', async () => {
    const el = timelineEl()!;
    const jumpSpy = vi.spyOn(el, 'jumpToFailure').mockReturnValue(true);
    const focusSpy = vi.spyOn(el, 'focusStep').mockReturnValue(true);

    emitLoad(makeLoadDetail());
    await fixture.whenStable();

    // Above-the-fold failure strip and initial auto-jump
    expect(root().querySelector('[data-testid="timeline-failure-strip"]')?.textContent).toContain(
      '640 ms (#10 braking pressure tracking)',
    );
    expect(jumpSpy).toHaveBeenCalledWith({ preserveZoom: false });
    expect(
      root().querySelector('[data-testid="timeline-visible-count"]')?.textContent?.trim(),
    ).toBe('4 / 4');
    expect(el.variables).toBeNull();

    // Simulate user cursor & step from element events
    el.dispatchEvent(new CustomEvent('cursorchange', { detail: { cursor: 580 }, bubbles: true }));
    el.dispatchEvent(new CustomEvent('stepselect', { detail: { step: 8 }, bubbles: true }));
    fixture.detectChanges();
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
      '580.0 ms',
    );
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
      '選んだ手順: 8',
    );

    // Preset: changed near failure includes ADAS.aeb.state as well as failing brake signals
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-preset-changed"]')!.click();
    fixture.detectChanges();
    expect(el.variables).toEqual([
      'ADAS.aeb.state',
      'Brake.hyd.pressure',
      'Brake.hyd.target_pressure',
    ]);
    // Preset: related to failure narrows to the two failing/related brake signals
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-preset-related"]')!.click();
    fixture.detectChanges();
    expect(el.variables).toEqual(['Brake.hyd.pressure', 'Brake.hyd.target_pressure']);
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-preset-all"]')!.click();
    fixture.detectChanges();
    expect(el.variables).toBeNull();

    // Search filter with custom clear button
    const search = root().querySelector<HTMLInputElement>(
      '[data-testid="timeline-signal-search"]',
    )!;
    search.value = 'Brake';
    search.dispatchEvent(new Event('input'));
    fixture.detectChanges();
    expect(el.variables).toEqual(['Brake.hyd.pressure', 'Brake.hyd.target_pressure']);
    root()
      .querySelector<HTMLButtonElement>('[data-testid="timeline-signal-search-clear"]')!
      .click();
    fixture.detectChanges();
    expect(el.variables).toBeNull();

    // Inline variablehide event adds to hidden-strip without wiping cursor or step
    const hideEv = new CustomEvent('variablehide', {
      detail: { variable: 'ADAS.aeb.active' },
      bubbles: true,
      cancelable: true,
    });
    el.dispatchEvent(hideEv);
    fixture.detectChanges();
    expect(hideEv.defaultPrevented).toBe(true);
    expect(root().querySelector('[data-testid="timeline-hidden-strip"]')?.textContent).toContain(
      'ADAS.aeb.active',
    );
    expect(el.variables).toEqual([
      'ADAS.aeb.state',
      'Brake.hyd.pressure',
      'Brake.hyd.target_pressure',
    ]);

    // Restore single hidden signal via inline chip
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-restore-var"]')!.click();
    fixture.detectChanges();
    expect(root().querySelector('[data-testid="timeline-hidden-strip"]')).toBeNull();
    expect(el.variables).toBeNull();

    // Cursor and step remained 100% preserved across all filtering and hide/restore steps
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
      '580.0 ms',
    );
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent).toContain(
      '選んだ手順: 8',
    );

    // Jump to failure button preserves zoom; pre-failure window presets and nav buttons use non-sticky methods
    const resetSpy = vi.spyOn(el, 'resetView').mockImplementation(() => undefined);
    const panSpy = vi.spyOn(el, 'pan').mockImplementation(() => undefined);
    const zoomOutSpy = vi.spyOn(el, 'zoomOut').mockImplementation(() => undefined);
    const unpinSpy = vi.spyOn(el, 'unpinCursor').mockImplementation(() => undefined);

    root().querySelector<HTMLButtonElement>('[data-testid="timeline-jump-failure"]')!.click();
    expect(jumpSpy).toHaveBeenLastCalledWith({ preserveZoom: true });

    root().querySelector<HTMLButtonElement>('[data-testid="timeline-window-150"]')!.click();
    expect(jumpSpy).toHaveBeenLastCalledWith({ leadMs: 150 });

    root().querySelector<HTMLButtonElement>('[data-testid="timeline-window-300"]')!.click();
    expect(jumpSpy).toHaveBeenLastCalledWith({ leadMs: 300 });

    // Without a post-failure tail (recordedTo === failure.at), Around failure button stays hidden
    expect(root().querySelector('[data-testid="timeline-window-around-failure"]')).toBeNull();

    root().querySelector<HTMLButtonElement>('[data-testid="timeline-window-all"]')!.click();
    expect(resetSpy).toHaveBeenCalledTimes(1);

    el.dispatchEvent(
      new CustomEvent('viewchange', { detail: { from: 490, to: 670 }, bubbles: true }),
    );
    fixture.detectChanges();
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-pan-left"]')!.click();
    expect(panSpy).toHaveBeenCalledWith(-0.25);
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-pan-right"]')!.click();
    expect(panSpy).toHaveBeenCalledWith(0.25);
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-zoom-out"]')!.click();
    expect(zoomOutSpy).toHaveBeenCalledTimes(1);
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-unpin"]')!.click();
    expect(unpinSpy).toHaveBeenCalledTimes(1);

    // showStep preserves zoom once loaded
    const comp = fixture.debugElement.children[0]!.componentInstance as TimelineView;
    comp.showStep(3);
    expect(focusSpy).toHaveBeenLastCalledWith(3, { preserveZoom: true });
  });

  it('keeps the element mounted across collapse/reopen, guards in-flight Run switches, and resets all state', async () => {
    const el = timelineEl()!;
    vi.spyOn(el, 'jumpToFailure').mockReturnValue(true);
    emitLoad(makeLoadDetail());
    await fixture.whenStable();

    // Populate search, preset, hidden signal, cursor, and step
    const search = root().querySelector<HTMLInputElement>(
      '[data-testid="timeline-signal-search"]',
    )!;
    search.value = 'ADAS';
    search.dispatchEvent(new Event('input'));
    root().querySelector<HTMLButtonElement>('[data-testid="timeline-preset-inputs"]')!.click();
    el.dispatchEvent(
      new CustomEvent('variablehide', {
        detail: { variable: 'Brake.hyd.pressure' },
        bubbles: true,
        cancelable: true,
      }),
    );
    el.dispatchEvent(new CustomEvent('cursorchange', { detail: { cursor: 410 }, bubbles: true }));
    el.dispatchEvent(new CustomEvent('stepselect', { detail: { step: 5 }, bubbles: true }));
    fixture.detectChanges();

    // Collapse & reopen: element is kept alive via [hidden] and all filter/cursor state survives
    const toggle = root().querySelector<HTMLButtonElement>('[data-testid="timeline-toggle"]')!;
    toggle.click();
    fixture.detectChanges();
    expect(timelineEl()).toBe(el);
    expect(root().querySelector<HTMLElement>('.analyzer-body')?.hidden).toBe(true);
    toggle.click();
    fixture.detectChanges();
    expect(root().querySelector<HTMLElement>('.analyzer-body')?.hidden).toBe(false);
    expect(
      root().querySelector<HTMLInputElement>('[data-testid="timeline-signal-search"]')?.value,
    ).toBe('ADAS');
    expect(root().querySelector('[data-testid="timeline-hidden-strip"]')).not.toBeNull();

    // Run switch (timelineUrl changes): clears cursor, step, search, preset, hidden signals, and meta
    fixture.componentInstance.autoExpand.set(true);
    fixture.componentInstance.initialStep.set(10);
    fixture.componentInstance.url.set('api/timeline/2');
    await fixture.whenStable();
    fixture.detectChanges();

    // In-flight stale events from Run 1 while Run 2 has not loaded yet must be ignored
    el.dispatchEvent(
      new CustomEvent('variablehide', {
        detail: { variable: 'ADAS.aeb.active' },
        bubbles: true,
        cancelable: true,
      }),
    );
    el.dispatchEvent(new CustomEvent('cursorchange', { detail: { cursor: 123 }, bubbles: true }));
    fixture.detectChanges();
    expect(root().querySelector('[data-testid="timeline-hidden-strip"]')).toBeNull();
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent).not.toContain(
      '123.0 ms',
    );

    // When Run 2 loads as an eventless timeline, stale initialStep (10) is synced away to null
    emitLoad(makeLoadDetail({ failure: null, status: 'passed', stepCount: 0 }));
    await fixture.whenStable();
    expect(root().querySelector('[data-testid="timeline-readout"]')?.textContent?.trim()).toBe('');
    expect(root().querySelector('[data-testid="timeline-hidden-strip"]')).toBeNull();
    expect(
      root().querySelector<HTMLInputElement>('[data-testid="timeline-signal-search"]')?.value,
    ).toBe('');
    expect(
      root().querySelector('[data-testid="timeline-preset-all"]')?.getAttribute('aria-pressed'),
    ).toBe('true');
    expect(
      root().querySelector('[data-testid="timeline-visible-count"]')?.textContent?.trim(),
    ).toBe('4 / 4');
  });

  it('shows Around failure quick-zoom button when recordedTo extends past failure.at and frames leadMs + trailMs', async () => {
    const el = timelineEl()!;
    const jumpSpy = vi.spyOn(el, 'jumpToFailure').mockReturnValue(true);

    emitLoad(
      makeLoadDetail({
        recordedTo: 940,
        end: 980,
      }),
    );
    await fixture.whenStable();

    const aroundBtn = root().querySelector<HTMLButtonElement>(
      '[data-testid="timeline-window-around-failure"]',
    );
    expect(aroundBtn).not.toBeNull();
    aroundBtn!.click();
    expect(jumpSpy).toHaveBeenLastCalledWith({ leadMs: 300, trailMs: 315 });
  });
});
