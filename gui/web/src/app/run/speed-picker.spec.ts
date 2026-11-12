import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Speed } from '../core/api';
import { SpeedPicker } from './speed-picker';

@Component({
  imports: [SpeedPicker],
  template: `<app-speed-picker
    [min]="0.01"
    [max]="1000"
    [(speed)]="speed"
    [(invalid)]="invalid"
  />`,
})
class Host {
  readonly speed = signal<Speed>(null);
  readonly invalid = signal(false);
}

describe('SpeedPicker', () => {
  let fixture: ComponentFixture<Host>;

  beforeEach(async () => {
    fixture = TestBed.createComponent(Host);
    await fixture.whenStable();
  });

  function element(): HTMLElement {
    return fixture.nativeElement as HTMLElement;
  }

  function factorInput(): HTMLInputElement | null {
    return element().querySelector('[data-testid="speed-factor"]');
  }

  async function chooseFactor(): Promise<HTMLInputElement> {
    const toggle = [...element().querySelectorAll('mat-button-toggle button')].find((b) =>
      b.textContent?.includes('倍率'),
    ) as HTMLButtonElement;
    toggle.click();
    await fixture.whenStable();
    const input = factorInput();
    if (!input) throw new Error('no factor field');
    return input;
  }

  async function type(input: HTMLInputElement, text: string): Promise<void> {
    input.value = text;
    input.dispatchEvent(new Event('input'));
    await fixture.whenStable();
  }

  it('keeps the factor field and its focus while a number starting with 1 is typed', async () => {
    const input = await chooseFactor();
    input.focus();
    for (const text of ['1', '1.', '1.5']) {
      await type(input, text);
      expect(factorInput()).toBe(input);
      expect(document.activeElement).toBe(input);
    }
    expect(fixture.componentInstance.speed()).toBe(1.5);
    await type(input, '10');
    expect(fixture.componentInstance.speed()).toBe(10);
    expect(fixture.componentInstance.invalid()).toBe(false);
  });

  it('says why a factor out of the range cannot be used', async () => {
    const input = await chooseFactor();
    await type(input, '5000');
    const error = element().querySelector('[data-testid="speed-error"]');
    expect(error?.textContent).toContain('0.01 から 1000');
    expect(error?.getAttribute('role')).toBe('alert');
    expect(input.getAttribute('aria-invalid')).toBe('true');
    expect(fixture.componentInstance.invalid()).toBe(true);
    await type(input, '');
    expect(fixture.componentInstance.invalid()).toBe(true);
    await type(input, '3');
    expect(fixture.componentInstance.invalid()).toBe(false);
    expect(error?.textContent?.trim()).toBe('');
  });
});
