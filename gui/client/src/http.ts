import { ServerProblem, readServerProblem } from './protocol';

/** One request to the GUI server. path is relative to the base (`api/run` and so on, with no leading /). */
export interface HttpRequest {
  method: 'GET' | 'POST';
  path: string;
  /** Body of a POST (sent as JSON). */
  body?: unknown;
  /** How long to wait for the answer (ms). Omitted: the transport's default (DEFAULT_TIMEOUT_MS for fetchTransport). */
  timeoutMs?: number;
  /** Forwarded to fetch() as `keepalive` so a request sent during `pagehide` can outlive the page. */
  keepalive?: boolean;
}

/** Default time to wait for an answer (ms). A check (`api/check`) waits for the server's check limit plus this. */
export const DEFAULT_TIMEOUT_MS = 30_000;

/** Outcome of a request: the answer's JSON on success, an HttpError on failure. */
export type HttpOutcome = { ok: true; body: unknown } | { ok: false; error: HttpError };

/**
 * Sends requests to the GUI server. Calls done exactly once, with the outcome. Adding the token (Authorization) and
 * Origin is the transport's job. The default is fetchTransport; it can be replaced with a framework's HTTP tool
 * (such as Angular's HttpClient).
 */
export type HttpTransport = (request: HttpRequest, done: (outcome: HttpOutcome) => void) => void;

/**
 * HTTP failure. Status 0 means the server did not answer (the request never arrived, or the wait timed out). problem
 * is the content of the server's error shape (`{"error": {...}}`). timedOut means the client gave up after the
 * timeout (the server may have received the request and still be working on it).
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

  /** Error for a request given up after the timeout. */
  static noAnswer(method: string, path: string, timeoutMs: number): HttpError {
    return new HttpError(
      0,
      `no answer from ${method} ${path} within ${timeoutMs / 1000} s`,
      null,
      true,
    );
  }

  /**
   * Builds the error from the answer's body (parsed JSON). problem is null unless the body has the server's error
   * shape.
   */
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
 * Gets the text to show a person for a failure: the server's message for a server error (in English, including how
 * to fix it), the status code for other HTTP failures.
 */
export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** Options of fetchTransport. */
export interface FetchTransportOptions {
  /** URL of the server (`http://127.0.0.1:<port>/`). Request paths are relative to it. */
  baseUrl: string;
  /** The token (what follows `#token=` in the URL the server printed), if required. A function is called for each request. */
  token?: string | null | (() => string | null);
  /**
   * Origin to send with POST. Browsers add it themselves (a value given here is not used). Outside the browser (Node
   * and so on), pass the server's own origin (the origin of baseUrl).
   */
  origin?: string;
  /** The fetch to use (default globalThis.fetch). */
  fetch?: typeof fetch;
  /** Default time to wait for an answer (ms; a request's timeoutMs takes precedence). */
  timeoutMs?: number;
}

/**
 * HttpTransport that sends with the standard fetch. Works in browsers and in Node 18 or later. Does not follow
 * redirects (so the token is never sent elsewhere). After the timeout, fails with an HttpError of status 0.
 */
export function fetchTransport(options: FetchTransportOptions): HttpTransport {
  const send = options.fetch ?? ((input, init) => globalThis.fetch(input, init));
  const rawToken = options.token;
  const token: () => string | null =
    typeof rawToken === 'function' ? rawToken : () => rawToken ?? null;
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
    if (request.keepalive !== undefined) init.keepalive = request.keepalive;
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
        // null if the body is not JSON (unless it has the error shape, the status code reports the failure)
        const body: unknown = await response.json().catch(() => null);
        clearTimeout(timer);
        // Timed out while reading the body: count it as no answer, not as an unreadable body
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
