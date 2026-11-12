import { ErrorSink, Readable, Writable, callEach, derived, rethrowLater } from './values';

/** closed: つながっていない（再接続を待っている間も）、connecting: つなぎに行っている、open: つながっている。 */
export type ConnectionState = 'connecting' | 'open' | 'closed';

/** 接続が閉じたときの、閉じるコードと理由の文。 */
export interface CloseInfo {
  code: number;
  reason: string;
}

/** サーバーが「後でもう一度」で閉じたときの閉じるコード。つなぎ直す（トークンを断ったのではない）。 */
export const TRY_AGAIN_LATER = 1013;

/**
 * 届いた通知をまとめて渡す間隔（ミリ秒）。ログが速く流れても、利用者への知らせは毎秒この回数まで。
 * 断られたこと（unauthorized）に気づくのも、まとめて渡すとき。サーバーが閉じた後のつなぎ直し（最初は 500 ms）より
 * 短いので、断られたトークンでつなぎ直すことはない（長くすると、1 回つなぎ直してから止まる）。
 */
export const BATCH_MS = 100;

/** WebSocket の出来事を受ける関数。SocketFactory に渡す。 */
export interface SocketHandlers {
  open(): void;
  /** 届いたテキストのメッセージ。 */
  message(text: string): void;
  /** 閉じた（開く前に閉じた、つながらなかった、も含む）。1 回だけ。 */
  close(code: number, reason: string): void;
}

/** 開いた WebSocket の、ライブラリが使う部分。 */
export interface SocketLike {
  send(text: string): void;
  close(): void;
}

/** WebSocket を開く関数。既定は webSocketFactory()。試験では偽物（testing の FakeWebSockets）に差し替える。 */
export type SocketFactory = (url: string, handlers: SocketHandlers) => SocketLike;

/** JSON でない通知で作り直した回数を数え直すまで、つながり続けている時間（ミリ秒）。 */
export const STEADY_MS = 30_000;

/** 開けなかったときの閉じるコード（握手まで届かなかった）。 */
const NOT_OPENED = 1006;

/**
 * 標準の WebSocket（ブラウザ、Web Worker、Node 22 以上の globalThis.WebSocket）で開く SocketFactory。
 * origin を渡すと、握手に Origin を付ける（Node で使うとき。Node の WebSocket の拡張の `{headers}` を使う。ブラウザと
 * Worker の WebSocket は 2 つ目の引数をサブプロトコルと読むので、そこでは渡さない）。開けなければ（URL が違う、など）
 * 閉じたことにして、理由を閉じた理由の文（`lastClose.reason`）に入れる。
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

/** 再接続の待ち時間。0.5 秒から倍にして、上限で頭打ち。attempt は 1 から。 */
export function backoffMs(attempt: number, maxMs = 5000): number {
  return Math.min(500 * 2 ** (attempt - 1), maxMs);
}

/** Connection の設定。 */
export interface ConnectionOptions {
  /** WebSocket の URL（`ws://127.0.0.1:<port>/ws`）。 */
  url: string;
  /** WebSocket を開く関数。 */
  socket: SocketFactory;
  /** つなぐたびに（再接続でも）、開いた直後に送るメッセージ（JSON にして送る）。つなぐときに呼ぶ。 */
  greeting: () => readonly unknown[];
  /** 再接続の待ち時間の上限。 */
  maxBackoffMs?: number;
  /** 届いた通知をまとめる間隔。 */
  batchMs?: number;
  /** 購読者が投げた例外を受ける。 */
  onError?: ErrorSink;
}

/** 接続の、読むための口（GuiClient.connection）。つなぐ、切るは GuiClient の connect() と close()。 */
export interface ClientConnection {
  /** 接続の状態。 */
  readonly state: Readable<ConnectionState>;
  /**
   * 開く前に閉じた（握手で断られたか、届かなかった）回数。ブラウザからは理由が見えないので、増えたら HTTP
   * （`GET api/app`）で理由を確かめる。
   */
  readonly failures: Readable<number>;
  /** 最後に閉じたときのコードと理由。閉じた後に通知が届いたら null に戻る（つながり直した）。 */
  readonly lastClose: Readable<CloseInfo | null>;
  /**
   * サーバーが接続を「後でもう一度」（1013）で閉じた理由の文（`too-many-connections`、`too-many-unauthenticated`）。
   * 間を置いてつなぎ直していて、つながり直したら null。
   */
  readonly tryAgainReason: Readable<string | null>;
  /**
   * 届いた通知（JSON を読んだもの。中身は確かめない。知らない type も入る）を、まとまりごとに受ける。返す関数で
   * 購読をやめる。
   */
  onBatch(listener: (batch: readonly unknown[]) => void): () => void;
  /** JSON として読めないメッセージ（テキスト）を受ける。そのとき接続は、間を置いてつなぎ直す。 */
  onUnreadable(listener: (text: string) => void): () => void;
}

/**
 * GUI サーバーとの WebSocket の接続（1 本）。枠組みに依らない。
 *
 * - connect() でつなぎ、close() で切る（つなぎ直さない）。close() の後に connect() すると、またつなぐ。
 *   つながっている間（つなぎ直しを待っている間も）の connect() は何もしない。
 * - 切れたら（エラーでも、サーバーが閉じても）backoffMs の間を置いてつなぎ直す。続けて切れるたびに待ち時間を延ばし
 *   （サーバーが 1013「後でもう一度」で閉じ続けても、詰めてつなぎ直さない）、読める通知が届いたら最初に戻す。
 * - つなぐたびに、開いた直後に greeting を送る（認証と、どこから再送してほしいか）。
 * - 届いた通知（JSON を読んだもの。中身は確かめない）を batchMs ごとにまとめて、onBatch の購読者へ渡す。
 *   中身を読むのは機能ごとの読み手（セッションの写し、など）。JSON として読めないものが届いたら、onUnreadable に
 *   知らせて、間を置いてつなぎ直す（hello から受け直す。迷ったら要約から作り直す）。
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
  /** 読める通知が届かないまま続けて切れた回数。 */
  private attempts = 0;
  /** 続けて作り直した回数（restart）。 */
  private restarts = 0;
  private restartAsked = false;
  /** 続けて JSON でない通知で作り直した回数。STEADY_MS の間つながり続けたら 0 に戻す。 */
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

  /** つなぐ（つながっているか、つなぎ直しを待っているなら何もしない）。 */
  connect(): void {
    if (this.wanted) return;
    this.wanted = true;
    this.open();
  }

  /** 切る。つなぎ直さない。まとめている途中の通知は捨てる。 */
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
   * いまの接続をやめて、間を置いてつなぎ直す（読み手が、通知から状態を作り直せなかったとき）。待ち時間は、続けて
   * つなぎ直すたびに延びる。
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

  /** まとめている通知を、待たずに渡す。 */
  flush(): void {
    clearTimeout(this.batchTimer);
    this.batchTimer = undefined;
    const batch = this.pending;
    this.pending = [];
    if (batch.length === 0) return;
    this.restartAsked = false;
    callEach(this.batchListeners, [batch], this.onError);
    // 読み手が作り直しを頼まずに受け終えた: 次の作り直しの待ち時間は最初から
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
      // 設定で渡した関数（認証のメッセージ、トークン、SocketFactory）が投げた: 知らせて、閉じたことにして、間を置いて
      // つなぎ直す（黙って止まらない）
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
      },
      message: (text) => {
        if (!current()) return;
        let message: unknown;
        try {
          message = JSON.parse(text);
        } catch {
          // 何が抜けたかわからない: 知らせて、hello から受け直す。待ち時間は、読めないものが来るたびに延び、
          // STEADY_MS の間つながり続けるまで戻さない（読める通知と読めない通知を交互に送る相手にも、詰めてつながない）
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
