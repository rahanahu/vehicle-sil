import { Writable, derived, shallowEqual } from './values';

describe('readable values', () => {
  it('tells each change to its subscribers, not the value they already have, until they leave', () => {
    const value = new Writable(1);
    const seen: number[] = [];
    const stop = value.subscribe((v) => seen.push(v));
    expect(seen).toEqual([]);
    value.set(2);
    value.set(2);
    value.update((v) => v + 1);
    stop();
    value.set(4);
    expect(seen).toEqual([2, 3]);
    expect(value.get()).toBe(4);
  });

  it('gives a read-only view that follows the value', () => {
    const value = new Writable('a');
    const view = value.readonly();
    expect('set' in view).toBe(false);
    const seen: string[] = [];
    view.subscribe((v) => seen.push(v));
    value.set('b');
    expect([view.get(), seen]).toEqual(['b', ['b']]);
  });

  it('still tells the others when one subscriber throws, and throws it again later', () => {
    const later: (() => void)[] = [];
    const spy = vi.spyOn(globalThis, 'queueMicrotask').mockImplementation((f) => later.push(f));
    try {
      const value = new Writable(0);
      const seen: number[] = [];
      value.subscribe(() => {
        throw new Error('a broken subscriber');
      });
      value.subscribe((v) => seen.push(v));
      value.set(1);
      expect(seen).toEqual([1]);
      expect(later).toHaveLength(1);
      expect(later[0]).toThrow('a broken subscriber');
    } finally {
      spy.mockRestore();
    }
  });

  it('hands what a subscriber throws to the error sink it was given', () => {
    const errors: unknown[] = [];
    const value = new Writable<number>(0, Object.is, (error) => errors.push(error));
    value.subscribe(() => {
      throw new Error('broken');
    });
    value.set(1);
    expect(errors.map(String)).toEqual(['Error: broken']);
  });

  it('still tells the others when the error sink itself throws', () => {
    const later: (() => void)[] = [];
    const spy = vi.spyOn(globalThis, 'queueMicrotask').mockImplementation((f) => later.push(f));
    try {
      const value = new Writable<number>(0, Object.is, () => {
        throw new Error('a broken sink');
      });
      const seen: number[] = [];
      value.subscribe(() => {
        throw new Error('broken');
      });
      value.subscribe((v) => seen.push(v));
      value.set(1);
      expect(seen).toEqual([1]);
      expect(later[0]).toThrow('a broken sink');
    } finally {
      spy.mockRestore();
    }
  });

  it('does not hand an old value late when a subscriber changes it during the news', () => {
    const value = new Writable(0);
    const seen: [string, number][] = [];
    value.subscribe((v) => {
      seen.push(['a', v]);
      if (v === 1) value.set(2);
    });
    value.subscribe((v) => seen.push(['b', v]));
    value.set(1);
    // b は 2 だけを受ける（1 を後から受けて、get() と食い違うことがない）
    expect(seen).toEqual([
      ['a', 1],
      ['a', 2],
      ['b', 2],
    ]);
    expect(value.get()).toBe(2);
  });

  it('still tells a subscriber that leaves during the news, that once', () => {
    const value = new Writable(0);
    const seen: string[] = [];
    let stopB = () => undefined as void;
    value.subscribe(() => {
      seen.push('a');
      stopB();
    });
    stopB = value.subscribe(() => seen.push('b'));
    value.set(1);
    value.set(2);
    expect(seen).toEqual(['a', 'b', 'a']);
  });

  it('derives a value from others, and tells only real changes', () => {
    const a = new Writable(1);
    const b = new Writable(2);
    const sum = derived([a, b], () => ({ even: (a.get() + b.get()) % 2 === 0 }), shallowEqual);
    const seen: boolean[] = [];
    sum.subscribe((v) => seen.push(v.even));
    a.set(3); // 5: odd, same as before
    b.set(3); // 6: even
    expect(seen).toEqual([true]);
    expect(sum.get()).toEqual({ even: true });
  });
});
