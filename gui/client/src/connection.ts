import { ErrorSink, Readable, Writable, callEach, derived, rethrowLater } from './values';

/** closed: not connected (also while waiting to reconnect), connecting: connecting, open: connected. */
export type ConnectionState = 'connecting' | 'open' | 'closed';

/** Close code and reason text of a closed connection. */
export interface CloseInfo {
  code: number;
  reason: string;
}

/** Close code the server uses for "try again later". The client reconnects (the token was not refused). */
export const TRY_AGAIN_LATER = 1013;

/**
 * Interval at which received notifications are passed on in batches (ms). However fast the log flows, subscribers are
 * notified at most this many times a second. A refusal (unauthorized) is also noticed when a batch is passed on. This
 * is shorter than the reconnect delay after the server closes (500 ms at first), so the client never reconnects with a
 * refused token (if it were longer, the client would reconnect once and then stop).
 */
export const BATCH_MS = 100;

/** Receives WebSocket events. Passed to SocketFactory. */
export interface SocketHandlers {
  open(): void;
  /** A text message received. */
  message(text: string): void;
  /** Closed (including closed before opening, and could not connect). Called once. */
  close(code: number, reason: string): void;
}

/** The part of an open WebSocket that the library uses. */
export interface SocketLike {
  send(text: string): void;
  close(): void;
}

/** Opens a WebSocket. Default webSocketFactory(). Tests replace it with a fake (FakeWebSockets of testing). */
export type SocketFactory = (url: string, handlers: SocketHandlers) => SocketLike;

/** How long the connection must stay up before the count of restarts caused by non-JSON notifications is reset (ms). */
export const STEADY_MS = 30_000;

/** Close code when the socket could not be opened (the handshake was never reached). */
const NOT_OPENED = 1006;

/**
 * SocketFactory that opens the standard WebSocket (browsers, Web Workers, globalThis.WebSocket of Node 22 or later).
 * With origin, the handshake carries Origin (for Node; uses the `{headers}` extension of Node's WebSocket; browser and
 * Worker WebSockets read a second argument as subprotocols, so it is not passed there). If the socket cannot be opened
 * (a wrong URL and so on), this counts as closed, with the cause in the close reason text (`lastClose.reason`).
 */
export function webSocketFactory(options: { origin?: string } = {}): SocketFactory {
  return (url, on) => {
    const WebSocketClass = globalThis.WebSocket as unknown as new (
      url: string,
      init?: unknown,
    ) => WebSocket;
    let socket: WebSocket;
    try {
      socket = options.origin
        ? new WebSocketClass(url, { headers: { Origin: options.origin } })
        : new WebSocketClass(url);
    } catch (error) {
      const reason = `cannot open the WebSocket: ${error instanceof Error ? error.message : String(error)}`;
      setTimeout(() => on.close(NOT_OPENED, reason), 0);
      return { send: () => undefined, close: () => undefined };
    }
    socket.onopen = () => on.open();
    socket.onmessage = (event: MessageEvent) =>
      on.message(typeof event.data === 'string' ? event.data : String(event.data));
    socket.onclose = (event: CloseEvent) => on.close(event.code ?? 0, event.reason ?? '');
    return { send: (text) => socket.send(text), close: () => socket.close() };
  };
}

/** Reconnect delay: 0.5 s, doubled each attempt, capped at the limit. attempt starts at 1. */
export function backoffMs(attempt: number, maxMs = 5000): number {
  return Math.min(500 * 2 ** (attempt - 1), maxMs);
}

/** Options of Connection. */
export interface ConnectionOptions {
  /** WebSocket URL (`ws://127.0.0.1:<port>/ws`). */
  url: string;
  /** Opens the WebSocket. */
  socket: SocketFactory;
  /** Message sent right after each open, reconnects included (sent as JSON). Called when connecting. */
  greeting: () => readonly unknown[];
  /**
   * Messages sent right after greeting (those that follow authentication, such as variable subscriptions). Called on
   * open (includes those added between connecting and opening).
   */
  afterGreeting?: () => readonly unknown[];
  /** Upper limit of the reconnect delay. */
  maxBackoffMs?: number;
  /** Interval for batching received notifications. */
  batchMs?: number;
  /** Receives what subscribers threw. */
  onError?: ErrorSink;
}

/**
 * Read side of the connection (GuiClient.connection). Connect and disconnect with GuiClient's connect() and close().
 */
export interface ClientConnection {
  /** Connection state. */
  readonly state: Readable<ConnectionState>;
  /**
   * Number of times the socket closed before opening (refused at the handshake, or never reached the server). The
   * browser does not show why, so when this grows, ask over HTTP (`GET api/app`).
   */
  readonly failures: Readable<number>;
  /** Code and reason of the last close. Back to null when a notification arrives after the close (reconnected). */
  readonly lastClose: Readable<CloseInfo | null>;
  /**
   * Reason text with which the server closed the connection as "try again later" (1013): `too-many-connections`,
   * `too-many-unauthenticated`. The client reconnects after a delay; null once reconnected.
   */
  readonly tryAgainReason: Readable<string | null>;
  /**
   * Receives the notifications that arrived (parsed JSON, contents unchecked, unknown types included), one batch at a
   * time. Call the returned function to unsubscribe.
   */
  onBatch(listener: (batch: readonly unknown[]) => void): () => void;
  /** Receives messages (text) that cannot be parsed as JSON. The connection then reconnects after a delay. */
  onUnreadable(listener: (text: string) => void): () => void;
}

/**
 * One WebSocket connection to the GUI server. No framework needed.
 *
 * - connect() connects and close() disconnects (no reconnect). connect() after close() connects again. connect() while
 *   connected (also while waiting to reconnect) does nothing.
 * - When the connection drops (an error or the server closing it), reconnects after backoffMs. Each consecutive drop
 *   lengthens the delay (so a server that keeps closing with 1013 "try again later" is not hammered with reconnects);
 *   a readable notification resets it.
 * - Sends greeting right after each open (authentication, and where to resend from).
 * - Batches the received notifications (parsed JSON, contents unchecked) every batchMs and passes them to the onBatch
 *   subscribers. Each feature's reader (the session copy and so on) reads the contents. When something arrives that
 *   cannot be parsed as JSON, notifies onUnreadable and reconnects after a delay (receiving again from hello; when in
 *   doubt, readers rebuild from the summary).
 */
export class Connection implements ClientConnection {
  private readonly onError: ErrorSink;
  private readonly stateValue: Writable<ConnectionState>;
  private readonly failureCount: Writable<number>;
  private readonly lastCloseValue: Writable<CloseInfo | null>;
  private readonly batchListeners = new Set<(batch: readonly unknown[]) => void>();
  private readonly unreadableListeners = new Set<(text: string) => void>();
  private socket: SocketLike | null = null;
  private wanted = false;
  /** Consecutive drops without a readable notification. */
  private attempts = 0;
  /** Consecutive restarts (restart). */
  private restarts = 0;
  private restartAsked = false;
  /** Consecutive restarts caused by non-JSON notifications. Reset to 0 after staying connected for STEADY_MS. */
  private garbled = 0;
  private steadyTimer: ReturnType<typeof setTimeout> | undefined;
  private retryTimer: ReturnType<typeof setTimeout> | undefined;
  private batchTimer: ReturnType<typeof setTimeout> | undefined;
  private pending: unknown[] = [];

  readonly state: Readable<ConnectionState>;
  readonly failures: Readable<number>;
  readonly lastClose: Readable<CloseInfo | null>;
  readonly tryAgainReason: Readable<string | null>;

  constructor(private readonly options: ConnectionOptions) {
    this.onError = options.onError ?? rethrowLater;
    this.stateValue = new Writable<ConnectionState>('closed', Object.is, this.onError);
    this.failureCount = new Writable<number>(0, Object.is, this.onError);
    this.lastCloseValue = new Writable<CloseInfo | null>(null, Object.is, this.onError);
    this.state = this.stateValue.readonly();
    this.failures = this.failureCount.readonly();
    this.lastClose = this.lastCloseValue.readonly();
    this.tryAgainReason = derived(
      [this.lastClose],
      () => {
        const close = this.lastClose.get();
        return close?.code === TRY_AGAIN_LATER ? close.reason || 'try-again-later' : null;
      },
      Object.is,
      this.onError,
    );
  }

  /** Connects (does nothing if connected or waiting to reconnect). */
  connect(): void {
    if (this.wanted) return;
    this.wanted = true;
    this.open();
  }

  /** Disconnects, without reconnecting. Drops notifications still being batched. */
  close(): void {
    this.wanted = false;
    this.restarts = 0;
    this.attempts = 0;
    this.garbled = 0;
    clearTimeout(this.retryTimer);
    clearTimeout(this.steadyTimer);
    this.dropPending();
    const socket = this.socket;
    this.socket = null;
    socket?.close();
    this.stateValue.set('closed');
  }

  /**
   * Drops the current connection and reconnects after a delay (when a reader could not rebuild its state from the
   * notifications). The delay grows with each consecutive restart.
   */
  restart(): void {
    this.restartAfter(backoffMs(++this.restarts, this.options.maxBackoffMs));
  }

  private restartAfter(delayMs: number): void {
    if (!this.wanted) return;
    this.restartAsked = true;
    this.dropPending();
    clearTimeout(this.steadyTimer);
    const socket = this.socket;
    this.socket = null;
    socket?.close();
    this.stateValue.set('closed');
    this.scheduleRetry(delayMs);
  }

  /**
   * Sends a notification to the server (one that follows authentication, such as a variable subscription). Sends only
   * while open and returns whether it sent. What could not be sent goes out with afterGreeting on the next open.
   */
  send(message: unknown): boolean {
    if (this.socket === null || this.stateValue.get() !== 'open') return false;
    try {
      this.socket.send(JSON.stringify(message));
      return true;
    } catch (error) {
      this.onError(error);
      return false;
    }
  }

  onBatch(listener: (batch: readonly unknown[]) => void): () => void {
    this.batchListeners.add(listener);
    return () => {
      this.batchListeners.delete(listener);
    };
  }

  onUnreadable(listener: (text: string) => void): () => void {
    this.unreadableListeners.add(listener);
    return () => {
      this.unreadableListeners.delete(listener);
    };
  }

  /** Passes on the batched notifications without waiting. */
  flush(): void {
    clearTimeout(this.batchTimer);
    this.batchTimer = undefined;
    const batch = this.pending;
    this.pending = [];
    if (batch.length === 0) return;
    this.restartAsked = false;
    callEach(this.batchListeners, [batch], this.onError);
    // The readers took the batch without asking for a restart: the next restart's delay starts over
    if (!this.restartAsked) this.restarts = 0;
  }

  private dropPending(): void {
    clearTimeout(this.batchTimer);
    this.batchTimer = undefined;
    this.pending = [];
  }

  private open(): void {
    try {
      this.openSocket();
    } catch (error) {
      // A function from the options (the authentication message, the token, SocketFactory) threw: report it, treat the
      // connection as closed and reconnect after a delay (never stop silently)
      this.onError(error);
      this.socket = null;
      this.lastCloseValue.set({
        code: NOT_OPENED,
        reason: `cannot open the WebSocket: ${error instanceof Error ? error.message : String(error)}`,
      });
      this.stateValue.set('closed');
      this.scheduleRetry(backoffMs(++this.attempts, this.options.maxBackoffMs));
    }
  }

  private openSocket(): void {
    const greeting = this.options.greeting();
    this.stateValue.set('connecting');
    let opened = false;
    let socket: SocketLike | null = null;
    const current = () => socket !== null && this.socket === socket;
    const handlers: SocketHandlers = {
      open: () => {
        if (!current()) return;
        opened = true;
        this.stateValue.set('open');
        clearTimeout(this.steadyTimer);
        this.steadyTimer = setTimeout(() => (this.garbled = 0), STEADY_MS);
        for (const message of greeting) socket?.send(JSON.stringify(message));
        try {
          for (const message of this.options.afterGreeting?.() ?? []) {
            socket?.send(JSON.stringify(message));
          }
        } catch (error) {
          this.onError(error);
        }
      },
      message: (text) => {
        if (!current()) return;
        let message: unknown;
        try {
          message = JSON.parse(text);
        } catch {
          // Unknown what was lost: report it and receive again from hello. The delay grows with each unreadable message
          // and is not reset until the connection stays up for STEADY_MS (so a peer that alternates readable and
          // unreadable notifications is not hammered with reconnects either)
          callEach(this.unreadableListeners, [text], this.onError);
          this.restartAfter(backoffMs(++this.garbled, this.options.maxBackoffMs));
          return;
        }
        this.attempts = 0;
        if (this.lastCloseValue.get() !== null) this.lastCloseValue.set(null);
        this.pending.push(message);
        this.batchTimer ??= setTimeout(() => this.flush(), this.options.batchMs ?? BATCH_MS);
      },
      close: (code, reason) => {
        if (!current()) return;
        this.socket = null;
        clearTimeout(this.steadyTimer);
        if (!opened) this.failureCount.update((n) => n + 1);
        this.lastCloseValue.set({ code, reason });
        this.stateValue.set('closed');
        this.scheduleRetry(backoffMs(++this.attempts, this.options.maxBackoffMs));
      },
    };
    socket = this.options.socket(this.options.url, handlers);
    this.socket = socket;
  }

  private scheduleRetry(delayMs: number): void {
    if (!this.wanted) return;
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => {
      if (this.wanted && this.socket === null) this.open();
    }, delayMs);
  }
}
