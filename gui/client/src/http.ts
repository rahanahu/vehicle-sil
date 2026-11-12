import { ServerProblem, readServerProblem } from './protocol';

/** GUI サーバーへの 1 つの要求。path は base からの相対（`api/run` など。先頭に / を付けない）。 */
export interface HttpRequest {
  method: 'GET' | 'POST';
  path: string;
  /** POST の本文（JSON にして送る）。 */
  body?: unknown;
  /** 応答を待つ時間（ミリ秒）。省くと HTTP の口の既定（fetchTransport は DEFAULT_TIMEOUT_MS）。 */
  timeoutMs?: number;
}

/** 応答を待つ時間の既定（ミリ秒）。照合（`api/check`）は、サーバーの照合の上限に、この時間を足して待つ。 */
export const DEFAULT_TIMEOUT_MS = 30_000;

/** 要求の結果。成功なら応答の JSON、失敗なら HttpError。 */
export type HttpOutcome = { ok: true; body: unknown } | { ok: false; error: HttpError };

/**
 * GUI サーバーへ要求を送る口。done は 1 回だけ、結果を持って呼ぶ。トークン（Authorization）と Origin を付けるのは
 * この口の役目。既定は fetchTransport。枠組みの HTTP の道具（Angular の HttpClient など）に差し替えられる。
 */
export type HttpTransport = (request: HttpRequest, done: (outcome: HttpOutcome) => void) => void;

/**
 * HTTP の失敗。status 0 はサーバーから答えがなかった（届かなかったか、待つ時間を過ぎた）。problem はサーバーの誤りの形
 * （`{"error": {...}}`）の中身。timedOut は、待つ時間を過ぎて諦めた（サーバーは要求を受けて、まだ働いているかもしれない）。
 */
export class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly statusText: string,
    readonly problem: ServerProblem | null,
    readonly timedOut = false,
  ) {
    super(
      problem ? problem.message : timedOut ? statusText : `HTTP ${status} ${statusText}`.trim(),
    );
    this.name = 'HttpError';
  }

  /** 待つ時間を過ぎて諦めた要求の誤り。 */
  static noAnswer(method: string, path: string, timeoutMs: number): HttpError {
    return new HttpError(
      0,
      `no answer from ${method} ${path} within ${timeoutMs / 1000} s`,
      null,
      true,
    );
  }

  /** 応答の本文（JSON を読んだもの）から作る。サーバーの誤りの形でなければ problem は null。 */
  static from(status: number, statusText: string, body: unknown): HttpError {
    let problem: ServerProblem | null = null;
    if (typeof body === 'object' && body !== null && 'error' in body) {
      try {
        problem = readServerProblem(body.error, 'response.error');
      } catch {
        problem = null;
      }
    }
    return new HttpError(status, statusText, problem);
  }
}

/**
 * 失敗から、人に見せる文を取り出す。サーバーの誤りなら、その message（英語。直し方を含む）、ほかの HTTP の失敗は
 * 状態の番号。
 */
export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** fetchTransport の設定。 */
export interface FetchTransportOptions {
  /** サーバーの URL（`http://127.0.0.1:<port>/`）。要求の path はここからの相対。 */
  baseUrl: string;
  /** トークン（サーバーが表示した URL の `#token=` の後ろ）。関数なら要求のたびに呼ぶ。 */
  token: string | (() => string | null);
  /**
   * POST に付ける Origin。ブラウザは自分で付ける（ここで渡しても使われない）。ブラウザの外（Node など）では
   * サーバー自身の origin（baseUrl の origin）を渡す。
   */
  origin?: string;
  /** 使う fetch（既定は globalThis.fetch）。 */
  fetch?: typeof fetch;
  /** 応答を待つ時間の既定（ミリ秒。要求の timeoutMs が優先）。 */
  timeoutMs?: number;
}

/**
 * 標準の fetch で送る HttpTransport。ブラウザと Node（18 以上）で動く。転送（リダイレクト）には従わない（トークンを
 * ほかへ送らない）。応答を待つ時間を過ぎたら、状態 0 の HttpError にする。
 */
export function fetchTransport(options: FetchTransportOptions): HttpTransport {
  const send = options.fetch ?? ((input, init) => globalThis.fetch(input, init));
  const token = typeof options.token === 'function' ? options.token : () => options.token as string;
  return (request, done) => {
    const headers: Record<string, string> = {};
    const current = token();
    if (current) headers['Authorization'] = `Bearer ${current}`;
    if (request.method === 'POST') {
      headers['Content-Type'] = 'application/json';
      if (options.origin) headers['Origin'] = options.origin;
    }
    const url = new URL(request.path, options.baseUrl).href;
    const timeoutMs = request.timeoutMs ?? options.timeoutMs ?? DEFAULT_TIMEOUT_MS;
    const abort = new AbortController();
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      abort.abort();
    }, timeoutMs);
    const init: RequestInit = {
      method: request.method,
      headers,
      redirect: 'error',
      signal: abort.signal,
    };
    if (request.method === 'POST') init.body = JSON.stringify(request.body ?? {});
    const fail = (error: unknown) =>
      done({
        ok: false,
        error: timedOut
          ? HttpError.noAnswer(request.method, request.path, timeoutMs)
          : new HttpError(0, errorMessage(error), null),
      });
    send(url, init)
      .then(async (response) => {
        // 本文が JSON でなければ null（誤りの形でなければ、状態の番号で知らせる）
        const body: unknown = await response.json().catch(() => null);
        clearTimeout(timer);
        // 本文を読んでいる途中で待つ時間を過ぎた: 読めなかった本文ではなく、答えがなかったとする
        if (timedOut) {
          fail(null);
          return;
        }
        done(
          response.ok
            ? { ok: true, body }
            : { ok: false, error: HttpError.from(response.status, response.statusText, body) },
        );
      }, fail)
      .finally(() => clearTimeout(timer));
  };
}
