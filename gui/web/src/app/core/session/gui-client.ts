import { HttpClient, HttpErrorResponse } from '@angular/common/http';
import {
  DOCUMENT,
  DestroyRef,
  ErrorHandler,
  InjectionToken,
  Signal,
  inject,
  signal,
} from '@angular/core';
import {
  ANSWER_TIMEOUT_MS,
  DEFAULT_TIMEOUT_MS,
  GuiClient,
  HttpError,
  HttpTransport,
  Readable,
  SocketFactory,
  createClient,
  errorMessage,
  webSocketFactory,
} from '@vehicle-sil/gui-client';
import { TimeoutError, timeout } from 'rxjs';
import { AuthToken } from '../auth/auth-token';

// クライアントのライブラリ（gui/client）を Angular につなぐところ。アプリ全体で 1 つのクライアントを作り、
// HTTP は HttpClient（interceptor がトークンを付ける）で送る。画面はここを直接使わず、包み（ServerConnection、
// ServerInfo、SimulationTime、RunCommands）の signal を読む。

/** WebSocket を開く関数。試験では偽物（`@vehicle-sil/gui-client/testing` の FakeWebSockets）に差し替える。 */
export const SOCKET_FACTORY = new InjectionToken<SocketFactory>('SOCKET_FACTORY', {
  providedIn: 'root',
  factory: () => webSocketFactory(),
});

/** 時刻の命令の答えを待つ時間（ミリ秒）を差し替える口（単体試験が短くする）。 */
export const ANSWER_TIMEOUT = new InjectionToken<number>('ANSWER_TIMEOUT', {
  factory: () => ANSWER_TIMEOUT_MS,
});

/**
 * HttpClient で送る HttpTransport。失敗は HttpError にする（サーバーの誤りの形なら、その文）。応答を待つ時間を
 * 過ぎたら、状態 0 の HttpError にする（fetchTransport と同じ）。
 */
export function httpClientTransport(http: HttpClient): HttpTransport {
  return (request, done) => {
    const timeoutMs = request.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const response =
      request.method === 'GET'
        ? http.get(request.path)
        : http.post(request.path, request.body ?? null);
    response.pipe(timeout(timeoutMs)).subscribe({
      next: (body) => done({ ok: true, body }),
      error: (error: unknown) =>
        done({
          ok: false,
          error:
            error instanceof HttpErrorResponse
              ? HttpError.from(error.status, error.statusText, error.error)
              : error instanceof TimeoutError
                ? HttpError.noAnswer(request.method, request.path, timeoutMs)
                : new HttpError(0, errorMessage(error), null),
        }),
    });
  };
}

/** アプリ全体で 1 つの GUI サーバーのクライアント。注入した側が connect() する（ServerConnection）。 */
export const GUI_CLIENT = new InjectionToken<GuiClient>('GUI_CLIENT', {
  providedIn: 'root',
  factory: () => {
    const auth = inject(AuthToken);
    const errors = inject(ErrorHandler);
    const client = createClient({
      baseUrl: inject(DOCUMENT).baseURI,
      token: () => auth.token(),
      http: httpClientTransport(inject(HttpClient)),
      socket: inject(SOCKET_FACTORY),
      answerTimeoutMs: inject(ANSWER_TIMEOUT),
      // 購読（signal に写すところ）が投げたものは、Angular の誤りの扱いへ
      onError: (error) => errors.handleError(error),
    });
    inject(DestroyRef).onDestroy(() => client.close());
    return client;
  },
});

/**
 * 読める値（ライブラリの状態）を signal にする。注入の文脈で呼ぶ（サービスの field など）。購読は、その注入の
 * 文脈が壊れるまで続く。
 */
export function readableSignal<T>(readable: Readable<T>): Signal<T> {
  const value = signal(readable.get());
  const stop = readable.subscribe((next) => value.set(next));
  inject(DestroyRef).onDestroy(stop);
  return value.asReadonly();
}
