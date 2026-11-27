import { HttpClient, HttpErrorResponse } from '@angular/common/http';
import {
  DOCUMENT,
  DestroyRef,
  ErrorHandler,
  InjectionToken,
  Signal,
  WritableSignal,
  computed,
  inject,
  signal,
} from '@angular/core';
import {
  ANSWER_TIMEOUT_MS,
  DEFAULT_TIMEOUT_MS,
  Front,
  FrontStatus,
  GuiClient,
  HttpError,
  HttpTransport,
  OperationName,
  Readable,
  Reason,
  SocketFactory,
  createFront,
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
    if (request.signal?.aborted) {
      done({ ok: false, error: new HttpError(0, 'aborted', null) });
      return;
    }
    let finished = false;
    let sub: { unsubscribe(): void } | null = null;
    const onAbort = () => {
      sub?.unsubscribe();
      finish({ ok: false, error: new HttpError(0, 'aborted', null) });
    };
    const finish = (outcome: Parameters<typeof done>[0]) => {
      if (finished) return;
      finished = true;
      request.signal?.removeEventListener('abort', onAbort);
      done(outcome);
    };
    const timeoutMs = request.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const response =
      request.method === 'GET'
        ? http.get(request.path)
        : http.post(request.path, request.body ?? null);
    request.signal?.addEventListener('abort', onAbort, { once: true });
    sub = response.pipe(timeout(timeoutMs)).subscribe({
      next: (body) => finish({ ok: true, body }),
      error: (error: unknown) =>
        finish({
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

/**
 * アプリ全体で 1 つの GUI サーバーの正面の口（gui-client の `createFront`）。実行の画面は、操作の可否と理由、状態、
 * 操作をここから読む。接続は GUI_CLIENT（`front.low`）と同じ 1 本。注入した側が connect() する（ServerConnection）。
 */
export const GUI_FRONT = new InjectionToken<Front>('GUI_FRONT', {
  providedIn: 'root',
  factory: () => {
    const auth = inject(AuthToken);
    const errors = inject(ErrorHandler);
    const front = createFront({
      baseUrl: inject(DOCUMENT).baseURI,
      token: () => auth.token(),
      http: httpClientTransport(inject(HttpClient)),
      socket: inject(SOCKET_FACTORY),
      answerTimeoutMs: inject(ANSWER_TIMEOUT),
      // 購読（signal に写すところ）が投げたものは、Angular の誤りの扱いへ
      onError: (error) => errors.handleError(error),
    });
    inject(DestroyRef).onDestroy(() => front.close());
    return front;
  },
});

/** アプリ全体で 1 つの GUI サーバーのクライアント（正面の口と同じ接続: `front.low`）。 */
export const GUI_CLIENT = new InjectionToken<GuiClient>('GUI_CLIENT', {
  providedIn: 'root',
  factory: () => inject(GUI_FRONT).low,
});

/** 操作の失敗と、その操作。 */
export interface OperationFailure {
  op: OperationName;
  /** 正面の口の `status.problem`（接続、異常終了、失敗した実行のこともある: 画面が code で分ける）。 */
  problem: Reason;
}

/**
 * このアプリが最後に送った操作（RunCommands と SimulationTime が送る前に書く）。正面の口の `status.problem` にある
 * 操作の失敗は、最後の操作のもの（次の操作で消える）なので、画面はこれで、どの場所に出すかを決める。
 */
const LAST_OPERATION = new InjectionToken<WritableSignal<OperationName | null>>('LAST_OPERATION', {
  providedIn: 'root',
  factory: () => signal(null),
});

/**
 * 操作 `op` を送る前に呼ぶ（それが最後の操作になる）。返す signal は、最後の操作が `ops` のどれかなら、それと
 * `status.problem`（なければ null）。注入の文脈で呼ぶ。
 */
export function lastOperation(
  status: Signal<FrontStatus>,
  ops: readonly OperationName[],
): { sending(op: OperationName): void; failure: Signal<OperationFailure | null> } {
  const last = inject(LAST_OPERATION);
  return {
    sending: (op) => last.set(op),
    failure: computed(() => {
      const op = last();
      const problem = status().problem;
      return op !== null && ops.includes(op) && problem !== null ? { op, problem } : null;
    }),
  };
}

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
