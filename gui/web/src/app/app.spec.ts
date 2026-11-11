import { signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { App } from './app';
import { Layout, WidthClass } from './core/layout/layout';

describe('App', () => {
  const widthClass = signal<WidthClass>('wide');

  beforeEach(async () => {
    widthClass.set('wide');
    await TestBed.configureTestingModule({
      imports: [App],
      providers: [provideRouter([]), { provide: Layout, useValue: { widthClass } }],
    }).compileComponents();
  });

  function query(element: HTMLElement, testId: string): HTMLElement | null {
    return element.querySelector(`[data-testid="${testId}"]`);
  }

  it('shows the title and a router outlet', async () => {
    const fixture = TestBed.createComponent(App);
    await fixture.whenStable();
    const element = fixture.nativeElement as HTMLElement;
    expect(query(element, 'app-title')?.textContent).toContain('vehicle-sil');
    expect(element.querySelector('router-outlet')).not.toBeNull();
  });

  it('switches the navigation with the width class', async () => {
    const fixture = TestBed.createComponent(App);
    const element = fixture.nativeElement as HTMLElement;
    const navMode = () => query(element, 'nav')?.getAttribute('data-nav-mode');

    await fixture.whenStable();
    expect(navMode()).toBe('full');
    expect(query(element, 'nav-toggle')).toBeNull();

    widthClass.set('medium');
    await fixture.whenStable();
    expect(navMode()).toBe('rail');
    expect(element.getAttribute('data-width')).toBe('medium');

    widthClass.set('compact');
    await fixture.whenStable();
    expect(navMode()).toBe('drawer');
    const toggle = query(element, 'nav-toggle');
    expect(toggle?.getAttribute('aria-expanded')).toBe('false');
    expect(toggle?.getAttribute('aria-label')).toBe('ナビゲーションを開く');

    toggle?.click();
    await fixture.whenStable();
    expect(toggle?.getAttribute('aria-expanded')).toBe('true');
    expect(toggle?.getAttribute('aria-label')).toBe('ナビゲーションを閉じる');
  });
});
