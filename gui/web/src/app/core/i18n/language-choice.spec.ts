import { DestroyRef, LOCALE_ID } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { LanguageChoice, PAGE_RELOAD } from './language-choice';

function setUp(locale: string) {
  const reload = vi.fn();
  TestBed.configureTestingModule({
    providers: [
      { provide: LOCALE_ID, useValue: locale },
      { provide: PAGE_RELOAD, useValue: reload },
    ],
  });
  return { choice: TestBed.inject(LanguageChoice), reload };
}

describe('LanguageChoice', () => {
  afterEach(() => {
    localStorage.clear();
    sessionStorage.clear();
  });

  it('knows the language shown from LOCALE_ID', () => {
    expect(setUp('en').choice.current).toBe('en');
    TestBed.resetTestingModule();
    expect(setUp('ja').choice.current).toBe('ja');
  });

  it('remembers another language, carries the kept values and reloads', () => {
    const { choice, reload } = setUp('ja');
    choice.keep('run', () => ({ file: 'a.yaml' }), TestBed.inject(DestroyRef));
    choice.choose('en');
    expect(localStorage.getItem('vehicle-sil-gui-language')).toBe('en');
    expect(JSON.parse(sessionStorage.getItem('vehicle-sil-gui-carry') ?? '')).toEqual({
      run: { file: 'a.yaml' },
    });
    expect(reload).toHaveBeenCalledOnce();
  });

  it('does nothing when the language shown is chosen', () => {
    const { choice, reload } = setUp('en');
    choice.choose('en');
    expect(localStorage.getItem('vehicle-sil-gui-language')).toBeNull();
    expect(reload).not.toHaveBeenCalled();
  });

  it('gives the carried values once, and forgets them on the next reload', () => {
    sessionStorage.setItem('vehicle-sil-gui-carry', JSON.stringify({ run: { file: 'a.yaml' } }));
    const { choice } = setUp('en');
    expect(sessionStorage.getItem('vehicle-sil-gui-carry')).toBeNull();
    expect(choice.take('run')).toEqual({ file: 'a.yaml' });
    expect(choice.take('run')).toBeUndefined();
  });

  it('ignores carried values it cannot read', () => {
    sessionStorage.setItem('vehicle-sil-gui-carry', '{not json');
    expect(setUp('en').choice.take('run')).toBeUndefined();
  });
});
