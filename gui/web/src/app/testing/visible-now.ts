// 試験用の Visibility: 観察された要素を、すぐ「見えている」と知らせる。
// 本物の IntersectionObserver の最初の知らせは、ブラウザーの描画の機会に本物の時間で届き、偽のタイマー
// （vi.useFakeTimers）では進まない。描画に頼ると、並行する試験で描画が遅れたとき、見張りが始まる前に expect が
// 走る。見えた・外れた順序を試験が決めたいときは、visibility.spec.ts のように偽の IntersectionObserver を使う。
import { Provider } from '@angular/core';
import { Visibility } from '../widgets/visibility';

export class VisibleNow implements Pick<Visibility, 'observe'> {
  private readonly listeners = new Set<(visible: boolean) => void>();

  observe(_element: Element, listener: (visible: boolean) => void): () => void {
    this.listeners.add(listener);
    listener(true);
    return () => void this.listeners.delete(listener);
  }

  /** 見え方が変わったと、観察している全員に知らせる（外れたときの動きを試すとき）。 */
  tell(visible: boolean): void {
    for (const each of [...this.listeners]) each(visible);
  }
}

/** TestBed の providers に足す: 部品を描く試験で、Visibility を VisibleNow に替える。 */
export const provideVisibleNow = (): Provider => ({ provide: Visibility, useClass: VisibleNow });
