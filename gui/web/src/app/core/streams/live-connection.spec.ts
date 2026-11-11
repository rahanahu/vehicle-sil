import {
  EnvironmentInjector,
  createEnvironmentInjector,
  runInInjectionContext,
} from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { FakeWebSockets } from './fake-web-socket';
import { backoffMs, liveConnection } from './live-connection';
import { latestValue, recentValues } from './stream-signals';

describe('liveConnection', () => {
  let fake: FakeWebSockets;

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    TestBed.configureTestingModule({ providers: [fake.provider] });
  });

  afterEach(() => vi.useRealTimers());

  /** 画面の代わり。destroy() で、画面が壊れたときと同じく購読が終わる。 */
  function screen() {
    return createEnvironmentInjector([], TestBed.inject(EnvironmentInjector));
  }

  it('connects on the first subscriber and resolves the URL against the base href', () => {
    const connection = TestBed.runInInjectionContext(() => liveConnection<number>('ws'));
    expect(fake.sockets).toHaveLength(0);
    expect(connection.state()).toBe('closed');

    const latest = runInInjectionContext(screen(), () => latestValue(connection.messages));
    expect(fake.sockets).toHaveLength(1);
    expect(fake.last().config.url).toMatch(/^ws:\/\/.*\/ws$/);
    expect(connection.state()).toBe('connecting');

    fake.last().open();
    fake.last<number>().send(42);
    expect(connection.state()).toBe('open');
    expect(latest()).toBe(42);
  });

  it('shares one socket and closes it after the last subscriber leaves', () => {
    const connection = TestBed.runInInjectionContext(() =>
      liveConnection<number>('ws', { lingerMs: 1000 }),
    );
    const first = screen();
    const second = screen();
    const latest = runInInjectionContext(first, () => latestValue(connection.messages));
    const history = runInInjectionContext(second, () => recentValues(connection.messages, 2));
    expect(fake.sockets).toHaveLength(1);

    for (const value of [1, 2, 3]) {
      fake.last<number>().send(value);
    }
    expect(latest()).toBe(3);
    expect(history()).toEqual([2, 3]);

    first.destroy();
    second.destroy();
    vi.advanceTimersByTime(999);
    expect(fake.last().closedByClient).toBe(false);
    vi.advanceTimersByTime(1);
    expect(fake.last().closedByClient).toBe(true);
  });

  it('reconnects with a growing delay and resets it after a message', () => {
    const connection = TestBed.runInInjectionContext(() => liveConnection<number>('ws'));
    runInInjectionContext(screen(), () => latestValue(connection.messages));

    fake.last().fail();
    expect(connection.state()).toBe('closed');
    vi.advanceTimersByTime(backoffMs(1) - 1);
    expect(fake.sockets).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(2);
    expect(connection.state()).toBe('connecting');

    fake.last().fail();
    vi.advanceTimersByTime(backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);

    // 通知が届けば、次の待ち時間は最初に戻る
    fake.last().open();
    fake.last<number>().send(7);
    fake.last().fail();
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(4);
  });

  it('sends the greeting each time it connects, built at that moment', () => {
    let from = 0;
    const connection = TestBed.runInInjectionContext(() =>
      liveConnection<number>('ws', { greeting: () => [{ type: 'auth', from }] }),
    );
    runInInjectionContext(screen(), () => latestValue(connection.messages));
    expect(fake.last().greeting).toEqual([{ type: 'auth', from: 0 }]);

    from = 5;
    fake.last().close();
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);
    expect(fake.last().greeting).toEqual([{ type: 'auth', from: 5 }]);
  });

  it('keeps a fixed-length history', () => {
    const connection = TestBed.runInInjectionContext(() => liveConnection<number>('ws'));
    const one = runInInjectionContext(screen(), () => recentValues(connection.messages, 1));
    const three = runInInjectionContext(screen(), () => recentValues(connection.messages, 3));
    for (const value of [1, 2, 3, 4]) {
      fake.last<number>().send(value);
    }
    expect(one()).toEqual([4]);
    expect(three()).toEqual([2, 3, 4]);
  });

  it('caps the delay', () => {
    expect([1, 2, 3, 4, 5].map((attempt) => backoffMs(attempt))).toEqual([
      500, 1000, 2000, 4000, 5000,
    ]);
  });
});
