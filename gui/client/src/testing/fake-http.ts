// Fake HTTP (HttpTransport) for tests. Do not import it from production code.
import { HttpError, HttpOutcome, HttpRequest, HttpTransport } from '../http';

/** One request sent. Answer it with flush (the client waits until then). */
export class FakeRequest {
  constructor(
    readonly request: HttpRequest,
    private readonly done: (outcome: HttpOutcome) => void,
  ) {}

  /**
   * Answers. A status of 400 or above is a failure (an HttpError with the server's message when the body has the
   * server's error shape). Status 0 means the request never arrived.
   */
  flush(
    body: unknown,
    answer: { status: number; statusText: string } = { status: 200, statusText: 'OK' },
  ): void {
    this.done(
      answer.status >= 200 && answer.status < 400
        ? { ok: true, body }
        : { ok: false, error: HttpError.from(answer.status, answer.statusText, body) },
    );
  }

  /** Fails without an answer (never arrived, or timed out: `HttpError.noAnswer`). */
  fail(error: HttpError): void {
    this.done({ ok: false, error });
  }
}

/** Fake HTTP. Pass `transport` as `http` to createClient. Holds the requests sent, in order, until answered. */
export class FakeHttp {
  readonly requests: FakeRequest[] = [];

  readonly transport: HttpTransport = (request, done) => {
    this.requests.push(new FakeRequest(request, done));
  };

  /** Takes the one unanswered request for that path (throws unless there is exactly one). */
  expectOne(path: string): FakeRequest {
    const found = this.requests.filter((r) => r.request.path === path);
    if (found.length !== 1) {
      throw new Error(`expected one request to ${path}, found ${found.length}`);
    }
    this.requests.splice(this.requests.indexOf(found[0]), 1);
    return found[0];
  }

  /** No unanswered request for that path. */
  expectNone(path: string): void {
    const found = this.requests.filter((r) => r.request.path === path);
    if (found.length > 0) throw new Error(`expected no request to ${path}, found ${found.length}`);
  }

  /** No unanswered request. */
  verify(): void {
    if (this.requests.length > 0) {
      const paths = this.requests.map((r) => r.request.path).join(', ');
      throw new Error(`requests not answered: ${paths}`);
    }
  }
}
