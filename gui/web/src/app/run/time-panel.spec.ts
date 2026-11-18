import { signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { SimulationTime } from '../core/session/simulation-time';
import { TimePanel } from './time-panel';

describe('TimePanel: the step field', () => {
  let fixture: ComponentFixture<TimePanel>;
  let steps: number[];

  beforeEach(async () => {
    steps = [];
    // 一時停止していて、ステップ送りができる時刻
    const time = {
      supported: signal(true),
      simMs: signal(0),
      speed: signal(1),
      actualSpeed: signal(null),
      paused: signal(true),
      paced: signal(false),
      behind: signal(false),
      lateness: signal(null),
      lastCommand: signal(null),
      waiting: signal(false),
      controls: signal({
        step: { ok: true, reason: null, caution: null, min_ms: 1, max_ms: 60000 },
        speed: { ok: true, reason: null, caution: null, min: 0.01, max: 1000 },
      }),
      error: signal(null),
      errorFromServer: signal(false),
      step: (ms: number) => Promise.resolve(steps.push(ms)).then(() => undefined),
      setSpeed: () => Promise.resolve(),
    };
    TestBed.configureTestingModule({
      providers: [{ provide: SimulationTime, useValue: time }],
    });
    fixture = TestBed.createComponent(TimePanel);
    await fixture.whenStable();
  });

  function field(): HTMLInputElement {
    return fixture.nativeElement.querySelector('[data-testid="step-ms"]');
  }

  function error(): HTMLElement {
    return fixture.nativeElement.querySelector('[data-testid="step-ms-error"]');
  }

  async function type(text: string): Promise<void> {
    field().value = text;
    field().dispatchEvent(new Event('input'));
    await fixture.whenStable();
  }

  async function submit(): Promise<void> {
    field().form!.requestSubmit();
    await fixture.whenStable();
  }

  it('steps on submit (the Enter of the field), full-width digits too, and stays on the page', async () => {
    await type('２５');
    let prevented = false;
    field().form!.addEventListener('submit', (e) => (prevented = e.defaultPrevented));
    await submit();
    expect(steps).toEqual([25]);
    expect(prevented).toBe(true);
  });

  it('does not step on the Enter that confirms an input-method conversion', async () => {
    await type('２５');
    field().dispatchEvent(
      new KeyboardEvent('keydown', { key: 'Enter', isComposing: true, bubbles: true }),
    );
    await fixture.whenStable();
    expect(steps).toEqual([]);
  });

  it('shows an error when the field is left or sent, not while it is typed', async () => {
    await type('0');
    expect(error().textContent?.trim()).toBe('');
    expect(field().getAttribute('aria-invalid')).toBe('false');
    expect(field().hasAttribute('aria-describedby')).toBe(false);

    field().dispatchEvent(new Event('blur'));
    await fixture.whenStable();
    expect(error().textContent).toContain('1 から 60000');
    expect(error().getAttribute('role')).toBe('status');
    expect(field().getAttribute('aria-invalid')).toBe('true');
    expect(field().getAttribute('aria-describedby')).toBe('step-ms-error');

    // 打ち直すと消える（まだ誤りでも）
    await type('1x');
    expect(error().textContent?.trim()).toBe('');
    expect(field().getAttribute('aria-invalid')).toBe('false');

    // 送ると出る。送らない
    await submit();
    expect(error().textContent).toContain('1 から 60000');
    expect(steps).toEqual([]);
  });
});
