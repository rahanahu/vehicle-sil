import { BATCH_MS, Connection, STEADY_MS, backoffMs, webSocketFactory } from './connection';
import { FakeWebSockets } from './testing/fake-web-socket';

describe('Connection', () => {
  let fake: FakeWebSockets;

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
  });

  afterEach(() => vi.useRealTimers());

  function connection(greeting: () => readonly unknown[] = () => []): Connection {
    return new Connection({ url: 'ws://127.0.0.1:1/ws', socket: fake.factory, greeting });
  }

  /** Receives a batch. */
  function batches(c: Connection): unknown[][] {
    const got: unknown[][] = [];
    c.onBatch((batch) => got.push([...batch]));
    return got;
  }

  it('connects only when asked, and opens the URL it was given', () => {
    const c = connection();
    expect(fake.sockets).toHaveLength(0);
    expect(c.state.get()).toBe('closed');
    c.connect();
    expect(fake.sockets).toHaveLength(1);
    expect(fake.last().url).toBe('ws://127.0.0.1:1/ws');
    expect(c.state.get()).toBe('connecting');
    fake.last().open();
    expect(c.state.get()).toBe('open');
  });

  it('does not create a socket when connecting is synchronously cancelled', () => {
    const c = connection();
    c.state.subscribe((state) => {
      if (state === 'connecting') c.close();
    });
    c.connect();
    expect(fake.sockets).toHaveLength(0);
    expect(c.state.get()).toBe('closed');
  });

  it('does not let a cancelled attempt replace the connection started by its subscriber', () => {
    const c = connection();
    let replaced = false;
    c.state.subscribe((state) => {
      if (state !== 'connecting' || replaced) return;
      replaced = true;
      c.close();
      c.connect();
    });
    c.connect();
    expect(fake.sockets).toHaveLength(1);
    fake.last().open();
    expect(c.state.get()).toBe('open');
  });

  it('closes a stale socket when its factory starts a replacement', () => {
    let replaced = false;
    const socket = (url: string, handlers: Parameters<typeof fake.factory>[1]) => {
      const result = fake.factory(url, handlers);
      if (!replaced) {
        replaced = true;
        c.close();
        c.connect();
      }
      return result;
    };
    const c = new Connection({ url: 'ws://127.0.0.1:1/ws', socket, greeting: () => [] });
    c.connect();
    expect(fake.sockets).toHaveLength(2);
    expect(fake.sockets[0].closedByClient).toBe(true);
    expect(fake.sockets[1].closedByClient).toBe(false);
    fake.sockets[1].open();
    expect(c.state.get()).toBe('open');
  });

  it('hands what arrives over in batches, at most every BATCH_MS', () => {
    const c = connection();
    const got = batches(c);
    c.connect();
    fake.last().open();
    for (const value of [1, 2, 3]) fake.last().send(value);
    expect(got).toEqual([]);
    vi.advanceTimersByTime(BATCH_MS - 1);
    expect(got).toEqual([]);
    vi.advanceTimersByTime(1);
    expect(got).toEqual([[1, 2, 3]]);
    fake.last().send(4);
    vi.advanceTimersByTime(BATCH_MS);
    expect(got).toEqual([[1, 2, 3], [4]]);
  });

  it('says so when a message is not JSON, and starts over from hello', () => {
    const c = connection();
    const got = batches(c);
    const unreadable: string[] = [];
    c.onUnreadable((text) => unreadable.push(text));
    c.connect();
    fake.last().open();
    fake.last().send({ type: 'hello' });
    fake.last().sendText('{not json');
    expect(unreadable).toEqual(['{not json']);
    expect(fake.last().closedByClient).toBe(true);
    expect(c.state.get()).toBe('closed');
    // What was batched before the unreadable message is dropped too (received again from hello)
    vi.advanceTimersByTime(BATCH_MS);
    expect(got).toEqual([]);
    vi.advanceTimersByTime(backoffMs(1) - BATCH_MS);
    expect(fake.sockets).toHaveLength(2);
    // With a peer that sends only unreadable messages, the delay keeps growing
    fake.last().open();
    fake.last().sendText('still not json');
    vi.advanceTimersByTime(backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);
  });

  it('does not restart a replacement opened by an unreadable-message subscriber', () => {
    const c = connection();
    c.connect();
    const old = fake.last();
    old.open();
    c.onUnreadable(() => {
      c.close();
      c.connect();
      fake.last().open();
    });
    old.sendText('not JSON');
    const replacement = fake.last();
    expect(replacement).not.toBe(old);
    expect(replacement.closedByClient).toBe(false);
    expect(c.state.get()).toBe('open');
  });

  it('does not mark a replacement closed from the previous close notification', () => {
    const c = connection();
    c.connect();
    const old = fake.last();
    old.open();
    c.lastClose.subscribe((close) => {
      if (!close) return;
      c.close();
      c.connect();
      fake.last().open();
    });
    old.fail();
    expect(fake.last()).not.toBe(old);
    expect(fake.last().closedByClient).toBe(false);
    expect(c.state.get()).toBe('open');
  });

  it('reconnects with a growing delay and resets it after a message', () => {
    const c = connection();
    c.connect();

    fake.last().fail();
    expect(c.state.get()).toBe('closed');
    vi.advanceTimersByTime(backoffMs(1) - 1);
    expect(fake.sockets).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(2);
    expect(c.state.get()).toBe('connecting');

    fake.last().fail();
    vi.advanceTimersByTime(backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);

    // Once a notification arrives, the next delay starts over
    fake.last().open();
    fake.last().send(7);
    fake.last().fail();
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(4);
  });

  it('counts the sockets closed before they opened, and remembers how the last one closed', () => {
    const c = connection();
    c.connect();
    fake.last().close(1006, '');
    expect(c.failures.get()).toBe(1);
    vi.advanceTimersByTime(backoffMs(1));
    fake.last().open();
    fake.last().close(1013, 'too-many-connections');
    expect(c.failures.get()).toBe(1);
    expect(c.lastClose.get()).toEqual({ code: 1013, reason: 'too-many-connections' });
    expect(c.tryAgainReason.get()).toBe('too-many-connections');
    // No notification has arrived yet, so the delay has grown
    vi.advanceTimersByTime(backoffMs(2));
    expect(fake.sockets).toHaveLength(3);
    fake.last().open();
    fake.last().send({ type: 'hello' });
    expect(c.lastClose.get()).toBeNull();
    expect(c.tryAgainReason.get()).toBeNull();
  });

  it('sends the greeting each time it connects, built when it connects', () => {
    let from = 0;
    const c = connection(() => [{ type: 'auth', from }]);
    c.connect();
    fake.last().open();
    expect(fake.last().greeting).toEqual([{ type: 'auth', from: 0 }]);

    from = 5;
    fake.last().close();
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);
    fake.last().open();
    expect(fake.last().greeting).toEqual([{ type: 'auth', from: 5 }]);
  });

  it('does not reconnect after close(), and drops what it was batching', () => {
    const c = connection();
    const got = batches(c);
    c.connect();
    fake.last().open();
    fake.last().send(1);
    c.close();
    expect(fake.last().closedByClient).toBe(true);
    expect(c.state.get()).toBe('closed');
    // Events of a closed connection are no longer counted
    fake.last().close(4401, 'unauthorized');
    vi.advanceTimersByTime(backoffMs(5) * 2);
    expect(fake.sockets).toHaveLength(1);
    expect(got).toEqual([]);
    expect(c.lastClose.get()).toBeNull();
  });

  it('starts over when a reader asks, waiting longer each time in a row', () => {
    const c = connection();
    c.onBatch(() => c.restart());
    c.connect();
    fake.last().open();
    fake.last().send(1);
    vi.advanceTimersByTime(BATCH_MS);
    expect(fake.last().closedByClient).toBe(true);
    expect(c.state.get()).toBe('closed');
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);

    fake.last().open();
    fake.last().send(2);
    vi.advanceTimersByTime(BATCH_MS + backoffMs(2) - 1);
    expect(fake.sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(fake.sockets).toHaveLength(3);
  });

  it('connects again after close(), and hands the batches over again', () => {
    const c = connection();
    const got = batches(c);
    c.connect();
    fake.last().open();
    fake.last().send(1);
    c.close(); // Disconnect while batching
    c.connect();
    expect(fake.sockets).toHaveLength(2);
    fake.last().open();
    fake.last().send(2);
    vi.advanceTimersByTime(BATCH_MS);
    expect(got).toEqual([[2]]);
  });

  it('does nothing on a second connect() while connected or waiting to reconnect', () => {
    const c = connection();
    c.connect();
    c.connect();
    expect(fake.sockets).toHaveLength(1);
    fake.last().fail();
    c.connect();
    expect(fake.sockets).toHaveLength(1);
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(2);
  });

  it('goes on when a listener throws, and hands the error to onError', () => {
    const errors: unknown[] = [];
    const c = new Connection({
      url: 'ws://127.0.0.1:1/ws',
      socket: fake.factory,
      greeting: () => [],
      onError: (error) => errors.push(error),
    });
    const got: unknown[][] = [];
    c.onBatch(() => {
      throw new Error('a broken listener');
    });
    c.onBatch((batch) => got.push([...batch]));
    c.onUnreadable(() => {
      throw new Error('another broken listener');
    });
    c.connect();
    fake.last().open();
    fake.last().send(1);
    vi.advanceTimersByTime(BATCH_MS);
    expect(got).toEqual([[1]]);
    fake.last().sendText('not json');
    // Even if it throws, the restart still goes ahead
    expect(fake.last().closedByClient).toBe(true);
    expect(errors.map(String)).toEqual([
      'Error: a broken listener',
      'Error: another broken listener',
    ]);
  });

  it('does not stop reconnecting silently when the greeting or the socket factory throws', () => {
    const errors: unknown[] = [];
    let token = () => 'secret';
    let broken = true;
    const c = new Connection({
      url: 'ws://127.0.0.1:1/ws',
      socket: (url, handlers) => {
        if (broken) throw new Error('no socket today');
        return fake.factory(url, handlers);
      },
      greeting: () => [{ type: 'auth', token: token() }],
      onError: (error) => errors.push(error),
    });
    c.connect();
    expect(c.state.get()).toBe('closed');
    expect(c.lastClose.get()?.reason).toBe('cannot open the WebSocket: no socket today');
    broken = false;
    token = () => {
      throw new Error('no token today');
    };
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(0);
    token = () => 'secret';
    vi.advanceTimersByTime(backoffMs(2));
    expect(fake.sockets).toHaveLength(1);
    expect(errors.map(String)).toEqual(['Error: no socket today', 'Error: no token today']);
  });

  it('waits longer each time messages that are not JSON come, even between readable ones', () => {
    const c = connection();
    c.connect();
    for (const [attempt, socketsAfter] of [
      [1, 2],
      [2, 3],
      [3, 4],
    ]) {
      fake.last().open();
      fake.last().send({ type: 'hello' });
      vi.advanceTimersByTime(BATCH_MS);
      fake.last().sendText('garbage');
      vi.advanceTimersByTime(backoffMs(attempt) - 1);
      expect(fake.sockets).toHaveLength(socketsAfter - 1);
      vi.advanceTimersByTime(1);
      expect(fake.sockets).toHaveLength(socketsAfter);
    }
    // After staying connected for a while, it starts over
    fake.last().open();
    vi.advanceTimersByTime(STEADY_MS);
    fake.last().sendText('garbage');
    vi.advanceTimersByTime(backoffMs(1));
    expect(fake.sockets).toHaveLength(5);
  });

  // The earlier design (a shared RxJS stream) connected on the first subscription and disconnected when the last one
  // left. This design has no such behavior (connect() and close() decide), so there is no test for it.

  it('caps the delay', () => {
    expect([1, 2, 3, 4, 5].map((attempt) => backoffMs(attempt))).toEqual([
      500, 1000, 2000, 4000, 5000,
    ]);
  });

  describe('webSocketFactory', () => {
    /** Fake globalThis.WebSocket. Remembers the arguments it was created with. */
    class Recorder {
      static made: unknown[][] = [];
      onopen: (() => void) | null = null;
      onmessage: ((event: { data: unknown }) => void) | null = null;
      onclose: ((event: { code: number; reason: string }) => void) | null = null;
      constructor(...args: unknown[]) {
        Recorder.made.push(args);
      }
      send(): void {
        // Sends nothing
      }
      close(): void {
        // Nothing to close
      }
    }

    beforeEach(() => {
      Recorder.made = [];
      vi.stubGlobal('WebSocket', Recorder);
    });
    afterEach(() => vi.unstubAllGlobals());

    const handlers = (closes: [number, string][] = []) => ({
      open: () => undefined,
      message: () => undefined,
      close: (code: number, reason: string) => closes.push([code, reason]),
    });

    it('puts the Origin into the handshake only when it is given (Node)', () => {
      webSocketFactory({ origin: 'http://127.0.0.1:8765' })('ws://127.0.0.1:8765/ws', handlers());
      webSocketFactory()('ws://127.0.0.1:8765/ws', handlers());
      expect(Recorder.made).toEqual([
        ['ws://127.0.0.1:8765/ws', { headers: { Origin: 'http://127.0.0.1:8765' } }],
        ['ws://127.0.0.1:8765/ws'],
      ]);
    });

    it('says why when it cannot open the WebSocket', () => {
      vi.stubGlobal(
        'WebSocket',
        class {
          constructor() {
            throw new SyntaxError('the subprotocol is not valid');
          }
        },
      );
      const closes: [number, string][] = [];
      webSocketFactory({ origin: 'http://127.0.0.1:1' })('ws://127.0.0.1:1/ws', handlers(closes));
      vi.advanceTimersByTime(0);
      expect(closes).toEqual([[1006, 'cannot open the WebSocket: the subprotocol is not valid']]);
    });
  });
});
