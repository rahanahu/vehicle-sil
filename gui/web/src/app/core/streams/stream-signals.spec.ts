import {
  EnvironmentInjector,
  createEnvironmentInjector,
  runInInjectionContext,
} from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { Subject } from 'rxjs';
import { latestValue, recentValues } from './stream-signals';

describe('stream signals', () => {
  /** 画面の代わり。destroy() で、画面が壊れたときと同じく購読が終わる。 */
  function screen() {
    return createEnvironmentInjector([], TestBed.inject(EnvironmentInjector));
  }

  it('shows the latest value, and stops following when the screen goes away', () => {
    const values = new Subject<number>();
    const view = screen();
    const latest = runInInjectionContext(view, () => latestValue(values));
    expect(latest()).toBeUndefined();
    values.next(42);
    expect(latest()).toBe(42);
    view.destroy();
    expect(values.observed).toBe(false);
  });

  it('keeps a fixed-length history', () => {
    const values = new Subject<number>();
    const one = runInInjectionContext(screen(), () => recentValues(values, 1));
    const three = runInInjectionContext(screen(), () => recentValues(values, 3));
    for (const value of [1, 2, 3, 4]) values.next(value);
    expect(one()).toEqual([4]);
    expect(three()).toEqual([2, 3, 4]);
  });
});
