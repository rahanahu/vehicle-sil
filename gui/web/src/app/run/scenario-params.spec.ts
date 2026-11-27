import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { CheckScenario, ScenarioParamValue } from '@vehicle-sil/gui-client';
import { ScenarioParamsPanel } from './scenario-params';

@Component({
  imports: [ScenarioParamsPanel],
  template: `<app-scenario-params
    [scenario]="scenario()"
    [(overrides)]="overrides"
    [locked]="locked()"
    (invalidChange)="invalid.set($event)"
  />`,
})
class Host {
  readonly scenario = signal<CheckScenario | null>(null);
  readonly overrides = signal<Readonly<Record<string, ScenarioParamValue>>>({});
  readonly locked = signal(false);
  readonly invalid = signal(false);
}

describe('ScenarioParamsPanel', () => {
  let fixture: ComponentFixture<Host>;

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  it('renders typed controls, provenance badges, missing emphasis, and preserves string types', async () => {
    fixture.componentInstance.scenario.set({
      file: 'scenarios/cases.yaml',
      name: 'cases',
      steps: 2,
      params: [
        { name: 'throttle', value: 50, from: 'case', type: 'integer' },
        { name: 'label', value: '定格', from: 'scenario', type: 'string' },
        { name: 'enabled', value: true, from: 'variant', type: 'boolean' },
      ],
      param_meta: [
        { name: 'code', type: 'string', description: 'leading-zero code' },
        { name: 'throttle', type: 'integer' },
        { name: 'label', type: 'string' },
        { name: 'enabled', type: 'boolean' },
      ],
      missing_params: ['code'],
    });
    await fixture.whenStable();

    const root = fixture.nativeElement as HTMLElement;
    expect(root.querySelector('[data-testid="params-missing-banner"]')?.textContent).toContain('1');
    expect(root.querySelector('[data-testid="param-row-code"]')?.getAttribute('data-missing')).toBe(
      'true',
    );
    expect(root.querySelector('[data-testid="param-badge-throttle"]')?.textContent?.trim()).toBe(
      'ケース',
    );

    // String parameter keeps "0123" as a string
    const codeInput = root.querySelector<HTMLInputElement>('[data-testid="param-input-code"]')!;
    codeInput.value = '0123';
    codeInput.dispatchEvent(new Event('input'));
    await fixture.whenStable();
    expect(fixture.componentInstance.overrides()['code']).toBe('0123');
    expect(typeof fixture.componentInstance.overrides()['code']).toBe('string');
    expect(root.querySelector('[data-testid="params-missing-banner"]')).toBeNull();

    // Integer parameter rejects non-integer input and emits invalidChange=true
    const thrInput = root.querySelector<HTMLInputElement>('[data-testid="param-input-throttle"]')!;
    thrInput.value = '12.5';
    thrInput.dispatchEvent(new Event('input'));
    await fixture.whenStable();
    expect(fixture.componentInstance.invalid()).toBe(true);
    expect(root.querySelector('[data-testid="param-error-throttle"]')).not.toBeNull();

    // Valid integer updates override as a JSON number and clears invalidChange
    thrInput.value = '75';
    thrInput.dispatchEvent(new Event('input'));
    await fixture.whenStable();
    expect(fixture.componentInstance.invalid()).toBe(false);
    expect(fixture.componentInstance.overrides()['throttle']).toBe(75);
    expect(typeof fixture.componentInstance.overrides()['throttle']).toBe('number');
    expect(root.querySelector('[data-testid="param-badge-throttle"]')?.textContent?.trim()).toBe(
      '上書き',
    );

    // Individual reset removes one override; Reset all clears the rest
    root.querySelector<HTMLButtonElement>('[data-testid="param-reset-throttle"]')?.click();
    await fixture.whenStable();
    expect(fixture.componentInstance.overrides()).toEqual({ code: '0123' });

    root.querySelector<HTMLButtonElement>('[data-testid="params-reset-all"]')?.click();
    await fixture.whenStable();
    expect(fixture.componentInstance.overrides()).toEqual({});
  });
});
