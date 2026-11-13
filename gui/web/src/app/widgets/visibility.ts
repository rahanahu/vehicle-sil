import { Injectable } from '@angular/core';

/**
 * 要素が画面に見えているか（少し外側も含める）を知らせる。部品が、見えている間だけ値を watch するため。
 * 監視は 1 つの IntersectionObserver でまとめる（部品が数百あっても 1 つ）。1 つの要素を何人が見てもよい（1 つの
 * 部品で bindVariable を 2 回呼ぶなど）。IntersectionObserver のない環境（単体試験の jsdom）では、いつも見えて
 * いるとする。
 */
@Injectable({ providedIn: 'root' })
export class Visibility {
  private readonly listeners = new Map<Element, Set<(visible: boolean) => void>>();
  /** 要素ごとの、最後に知った見え方（後から見始めた人に、すぐ知らせる）。 */
  private readonly last = new Map<Element, boolean>();
  private observer: IntersectionObserver | null = null;

  /** 見えているかが変わるたびに listener を呼ぶ（最初の 1 回を含む）。返す関数でやめる。 */
  observe(element: Element, listener: (visible: boolean) => void): () => void {
    if (typeof IntersectionObserver === 'undefined') {
      listener(true);
      return () => undefined;
    }
    this.observer ??= new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          this.last.set(entry.target, entry.isIntersecting);
          for (const each of [...(this.listeners.get(entry.target) ?? [])]) {
            each(entry.isIntersecting);
          }
        }
      },
      // 少し手前から watch を始める（スクロールしてきたときに、値がもう届いているように）
      { rootMargin: '200px 0px' },
    );
    let set = this.listeners.get(element);
    if (!set) {
      set = new Set();
      this.listeners.set(element, set);
      this.observer.observe(element);
    } else if (this.last.has(element)) {
      listener(this.last.get(element) === true);
    }
    set.add(listener);
    return () => {
      const mine = this.listeners.get(element);
      if (!mine?.delete(listener) || mine.size > 0) return;
      this.listeners.delete(element);
      this.last.delete(element);
      this.observer?.unobserve(element);
    };
  }
}
