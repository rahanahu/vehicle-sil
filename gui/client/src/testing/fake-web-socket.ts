// Fake WebSocket (SocketFactory) for tests. Do not import it from production code.
import { SocketFactory, SocketHandlers } from '../connection';

/** One fake connection. The test makes it open, receive server notifications and drop. */
export class FakeSocket {
  /** The client closed it. */
  closedByClient = false;
  /** Messages the client sent (parsed JSON), including the authentication right after opening. */
  readonly sent: unknown[] = [];

  constructor(
    readonly url: string,
    private readonly handlers: SocketHandlers,
  ) {}

  /** The message the client sent right after opening (authentication). Empty before opening. */
  get greeting(): readonly unknown[] {
    return this.sent;
  }

  open(): void {
    this.handlers.open();
  }

  /** A notification from the server (passed as JSON). */
  send(value: unknown): void {
    this.handlers.message(JSON.stringify(value));
  }

  /** Text from the server (to test something that is not JSON). */
  sendText(text: string): void {
    this.handlers.message(text);
  }

  /** The connection dropped (closed with an error). */
  fail(): void {
    this.handlers.close(1006, '');
  }

  /** The server closed it. code and reason are the close code and the reason text. */
  close(code = 1000, reason = ''): void {
    this.handlers.close(code, reason);
  }
}

/** Holds the fake connections opened, in order. Pass `factory` as `socket` to createClient. */
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

  /** The connection opened last. */
  last(): FakeSocket {
    const socket = this.sockets.at(-1);
    if (!socket) {
      throw new Error('no WebSocket has been opened');
    }
    return socket;
  }
}
