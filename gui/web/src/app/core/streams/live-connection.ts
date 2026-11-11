import { DOCUMENT, InjectionToken, Signal, inject, signal } from '@angular/core';
import { Observable, defer, repeat, retry, share, timer } from 'rxjs';
import { WebSocketSubjectConfig, webSocket } from 'rxjs/webSocket';
import { webSocketUrl } from '../urls';

/** WebSocket を開く関数。試験では偽物に差し替える。 */
export type WebSocketFactory = <T>(config: WebSocketSubjectConfig<T>) => Observable<T>;

export const WEB_SOCKET_FACTORY = new InjectionToken<WebSocketFactory>('WEB_SOCKET_FACTORY', {
  providedIn: 'root',
  factory: () => (config) => webSocket(config),
});

/** closed: つながっていない（再接続を待っている間も）、connecting: つなぎに行っている、open: つながっている。 */
export type ConnectionState = 'connecting' | 'open' | 'closed';

export interface LiveConnection<T> {
  /** 接続の状態。 */
  readonly state: Signal<ConnectionState>;
  /** 届いた通知の流れ。購読する人がいる間だけつながる（共有）。画面へは stream-signals.ts の関数で signal にして渡す。 */
  readonly messages: Observable<T>;
}

export interface LiveConnectionOptions {
  /** 最後の購読が終わってから切るまでの時間。画面を切り替えたときにつなぎ直さないため。 */
  lingerMs?: number;
  /** 再接続の待ち時間の上限。 */
  maxBackoffMs?: number;
}

/** 再接続の待ち時間。0.5 秒から倍にして、上限で頭打ち。attempt は 1 から。 */
export function backoffMs(attempt: number, maxMs = 5000): number {
  return Math.min(500 * 2 ** (attempt - 1), maxMs);
}

/**
 * base href からの相対の path（例: 'ws'）への WebSocket の接続を作る。注入の文脈で呼ぶ（サービスの field など）。
 *
 * - 作っただけではつながらない。最初の購読でつなぎ、最後の購読が終わって lingerMs たったら切る。
 * - 切れたら（エラーでも、サーバーが閉じても）backoffMs の間を置いてつなぎ直す。通知が届いたら待ち時間を戻す。
 */
export function liveConnection<T>(
  path: string,
  options: LiveConnectionOptions = {},
): LiveConnection<T> {
  const { lingerMs = 1000, maxBackoffMs = 5000 } = options;
  const factory = inject(WEB_SOCKET_FACTORY);
  const url = webSocketUrl(inject(DOCUMENT).baseURI, path);
  const state = signal<ConnectionState>('closed');

  const messages = defer(() => {
    state.set('connecting');
    return factory<T>({
      url,
      openObserver: { next: () => state.set('open') },
      closeObserver: { next: () => state.set('closed') },
    });
  }).pipe(
    retry({
      delay: (_error, attempt) => timer(backoffMs(attempt, maxBackoffMs)),
      resetOnSuccess: true,
    }),
    repeat({ delay: () => timer(backoffMs(1, maxBackoffMs)) }),
    share({ resetOnRefCountZero: () => timer(lingerMs) }),
  );

  return { state: state.asReadonly(), messages };
}
