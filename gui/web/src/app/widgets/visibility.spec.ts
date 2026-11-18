import { TestBed } from '@angular/core/testing';
import { Visibility } from './visibility';

/**
 * 偽の IntersectionObserver: 試験が、見えたか・外れたかを知らせる。本物のブラウザーでも偽物を使うのは、見えた・外れた
 * の順序と回数を試験が決めるため（本物は描画の後に非同期で知らせ、順序を決められない）。
 */
class FakeObserver {
  static last: FakeObserver | null = null;
  readonly observed = new Set<Element>();
  constructor(
    private readonly callback: (entries: { target: Element; isIntersecting: boolean }[]) => void,
  ) {
    FakeObserver.last = this;
  }
  observe(element: Element) {
    this.observed.add(element);
  }
  unobserve(element: Element) {
    this.observed.delete(element);
  }
  tell(target: Element, isIntersecting: boolean) {
    this.callback([{ target, isIntersecting }]);
  }
}

describe('Visibility', () => {
  beforeEach(() => vi.stubGlobal('IntersectionObserver', FakeObserver));
  afterEach(() => vi.unstubAllGlobals());

  it('tells everyone who watches one element, and stops observing it after the last one', () => {
    const visibility = TestBed.inject(Visibility);
    const element = document.createElement('div');
    const a: boolean[] = [];
    const b: boolean[] = [];
    const stopA = visibility.observe(element, (v) => a.push(v));
    const observer = FakeObserver.last as FakeObserver;
    observer.tell(element, true);
    // 後から見始めた人には、いまの見え方をすぐ知らせる
    const stopB = visibility.observe(element, (v) => b.push(v));
    expect([a, b]).toEqual([[true], [true]]);
    observer.tell(element, false);
    expect([a, b]).toEqual([
      [true, false],
      [true, false],
    ]);
    stopA();
    expect(observer.observed.has(element)).toBe(true);
    observer.tell(element, true);
    expect([a, b]).toEqual([
      [true, false],
      [true, false, true],
    ]);
    stopB();
    expect(observer.observed.has(element)).toBe(false);
  });
});
