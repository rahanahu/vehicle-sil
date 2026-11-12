import { Signal } from '@angular/core';
import { toObservable, toSignal } from '@angular/core/rxjs-interop';
import { Observable, map, of, scan, startWith, switchMap, timer } from 'rxjs';

// 流れを画面の端で signal にする関数。コンポーネントの field で呼ぶ（注入の文脈）。
// 購読はそのコンポーネントが生きている間だけ続き、壊れると止まる。

/** 届いた最新の値。まだ届いていなければ undefined。 */
export function latestValue<T>(source: Observable<T>): Signal<T | undefined> {
  return toSignal(source);
}

/** 直近 size 個の値（古い順）。グラフのための固定長の履歴。 */
export function recentValues<T>(source: Observable<T>, size: number): Signal<readonly T[]> {
  return toSignal(
    source.pipe(
      scan(
        (history: readonly T[], value: T) => [
          ...history.slice(Math.max(0, history.length - size + 1)),
          value,
        ],
        [],
      ),
    ),
    { initialValue: [] },
  );
}

/**
 * enabled が偽から真になって ms たつまでは偽、それ以後は enabled と同じ（押せるようになったばかりの操作を、
 * 少しの間だけ押しても何もしないようにする）。
 */
export function settledAfter(enabled: Signal<boolean>, ms: number): Signal<boolean> {
  return toSignal(
    toObservable(enabled).pipe(
      switchMap((on) =>
        on
          ? timer(ms).pipe(
              map(() => true),
              startWith(false),
            )
          : of(false),
      ),
    ),
    { initialValue: false },
  );
}
