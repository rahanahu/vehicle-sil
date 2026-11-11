import { BreakpointObserver } from '@angular/cdk/layout';
import { Signal, inject } from '@angular/core';
import { toSignal } from '@angular/core/rxjs-interop';
import { map } from 'rxjs';

/**
 * メディアクエリのそれぞれが当たっているかを signal で返す。注入の文脈で呼ぶ。
 * 例: `mediaQueries({ wide: '(min-width: 1200px)' })()` → `{ wide: true }`
 */
export function mediaQueries<K extends string>(
  queries: Record<K, string>,
): Signal<Record<K, boolean>> {
  const entries = Object.entries(queries) as [K, string][];
  return toSignal(
    inject(BreakpointObserver)
      .observe(entries.map(([, query]) => query))
      .pipe(
        map(
          (state) =>
            Object.fromEntries(
              entries.map(([key, query]) => [key, state.breakpoints[query] ?? false]),
            ) as Record<K, boolean>,
        ),
      ),
    { requireSync: true },
  );
}
