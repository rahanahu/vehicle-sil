// 試験のための偽の HTTP（HttpTransport）。本番のコードからは import しない。
import { HttpError, HttpOutcome, HttpRequest, HttpTransport } from '../http';

/** 送られた要求 1 つ。flush で答える（答えるまで、クライアントは待つ）。 */
export class FakeRequest {
  constructor(
    readonly request: HttpRequest,
    private readonly done: (outcome: HttpOutcome) => void,
  ) {}

  /**
   * 答える。status が 400 以上なら失敗（本文がサーバーの誤りの形なら、その文の HttpError）。status 0 は届かなかった。
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

  /** 答えずに失敗させる（届かなかった、待つ時間を過ぎた: `HttpError.noAnswer`）。 */
  fail(error: HttpError): void {
    this.done({ ok: false, error });
  }
}

/** 偽の HTTP。createClient の `http` に transport を渡す。送られた要求を、答えるまで順に持つ。 */
export class FakeHttp {
  readonly requests: FakeRequest[] = [];

  readonly transport: HttpTransport = (request, done) => {
    this.requests.push(new FakeRequest(request, done));
  };

  /** その path の、まだ答えていない要求を 1 つ取り出す（ちょうど 1 つでなければ投げる）。 */
  expectOne(path: string): FakeRequest {
    const found = this.requests.filter((r) => r.request.path === path);
    if (found.length !== 1) {
      throw new Error(`expected one request to ${path}, found ${found.length}`);
    }
    this.requests.splice(this.requests.indexOf(found[0]), 1);
    return found[0];
  }

  /** その path の、まだ答えていない要求がない。 */
  expectNone(path: string): void {
    const found = this.requests.filter((r) => r.request.path === path);
    if (found.length > 0) throw new Error(`expected no request to ${path}, found ${found.length}`);
  }

  /** まだ答えていない要求がない。 */
  verify(): void {
    if (this.requests.length > 0) {
      const paths = this.requests.map((r) => r.request.path).join(', ');
      throw new Error(`requests not answered: ${paths}`);
    }
  }
}
