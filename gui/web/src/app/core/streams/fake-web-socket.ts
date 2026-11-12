// 単体試験のための偽の WebSocket。本番のコードからは import しない。
import { Provider } from '@angular/core';
import { Observable, Subscriber } from 'rxjs';
import { WebSocketSubjectConfig } from 'rxjs/webSocket';
import { WEB_SOCKET_FACTORY, WebSocketFactory } from './live-connection';

/** 偽の接続 1 本。開く、通知、壊れるを試験から起こす。 */
export class FakeSocket<T = unknown> {
  closedByClient = false;

  constructor(
    readonly config: WebSocketSubjectConfig<T>,
    /** 開いた直後に送ったメッセージ（liveConnection の greeting）。 */
    readonly greeting: readonly unknown[],
    private readonly subscriber: Subscriber<T>,
  ) {}

  open(): void {
    this.config.openObserver?.next(new Event('open'));
  }

  send(value: T): void {
    this.subscriber.next(value);
  }

  fail(): void {
    this.config.closeObserver?.next(new Event('close') as CloseEvent);
    this.subscriber.error(new Error('connection lost'));
  }

  /** サーバーが閉じた（エラーでない終わり）。code と reason は閉じるコードと理由の文。 */
  close(code = 1000, reason = ''): void {
    this.config.closeObserver?.next(new CloseEvent('close', { code, reason, wasClean: true }));
    this.subscriber.complete();
  }
}

/** 開かれた偽の接続を順に持つ。`providers: [fake.provider]` で WEB_SOCKET_FACTORY を差し替える。 */
export class FakeWebSockets {
  readonly sockets: FakeSocket[] = [];

  private readonly factory: WebSocketFactory = <T>(
    config: WebSocketSubjectConfig<T>,
    greeting: readonly unknown[],
  ) =>
    new Observable<T>((subscriber) => {
      const socket = new FakeSocket<T>(config, greeting, subscriber);
      this.sockets.push(socket as FakeSocket);
      return () => (socket.closedByClient = true);
    });

  readonly provider: Provider = { provide: WEB_SOCKET_FACTORY, useValue: this.factory };

  /** 最後に開かれた接続。 */
  last<T>(): FakeSocket<T> {
    const socket = this.sockets.at(-1);
    if (!socket) {
      throw new Error('no WebSocket has been opened');
    }
    return socket as FakeSocket<T>;
  }
}
