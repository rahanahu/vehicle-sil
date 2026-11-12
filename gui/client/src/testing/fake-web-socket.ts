// 試験のための偽の WebSocket（SocketFactory）。本番のコードからは import しない。
import { SocketFactory, SocketHandlers } from '../connection';

/** 偽の接続 1 本。開く、サーバーからの通知、切れるを試験から起こす。 */
export class FakeSocket {
  /** クライアントが閉じた。 */
  closedByClient = false;
  /** クライアントが送ったメッセージ（JSON を読んだもの）。開いた直後の認証も入る。 */
  readonly sent: unknown[] = [];

  constructor(
    readonly url: string,
    private readonly handlers: SocketHandlers,
  ) {}

  /** 開いた直後にクライアントが送ったメッセージ（認証）。開く前は空。 */
  get greeting(): readonly unknown[] {
    return this.sent;
  }

  open(): void {
    this.handlers.open();
  }

  /** サーバーからの通知（JSON にして渡す）。 */
  send(value: unknown): void {
    this.handlers.message(JSON.stringify(value));
  }

  /** サーバーからのテキスト（JSON でないものを試すとき）。 */
  sendText(text: string): void {
    this.handlers.message(text);
  }

  /** 切れた（エラーで閉じた）。 */
  fail(): void {
    this.handlers.close(1006, '');
  }

  /** サーバーが閉じた。code と reason は閉じるコードと理由の文。 */
  close(code = 1000, reason = ''): void {
    this.handlers.close(code, reason);
  }
}

/** 開かれた偽の接続を順に持つ。createClient の `socket` に factory を渡す。 */
export class FakeWebSockets {
  readonly sockets: FakeSocket[] = [];

  readonly factory: SocketFactory = (url, handlers) => {
    const socket = new FakeSocket(url, handlers);
    this.sockets.push(socket);
    return {
      send: (text) => socket.sent.push(JSON.parse(text)),
      close: () => {
        socket.closedByClient = true;
      },
    };
  };

  /** 最後に開かれた接続。 */
  last(): FakeSocket {
    const socket = this.sockets.at(-1);
    if (!socket) {
      throw new Error('no WebSocket has been opened');
    }
    return socket;
  }
}
