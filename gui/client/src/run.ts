import { HttpError, HttpTransport, errorMessage } from './http';
import { ProtocolError, RunRequest, SessionSummary, readSessionResponse } from './protocol';
import { SessionFeed } from './session';
import { ErrorSink, Readable, Writable, derived, rethrowLater } from './values';

/** 停止の操作: 順に止める、待たずに止める。 */
export type StopAction = 'stop' | 'force';

/**
 * 答えが来なかった（待つ時間を過ぎた）操作の文。断られたのではなく、サーバーはしたかもしれない: 開始なら、実行が
 * 始まったかは WebSocket の状態（セッションの写し）でわかる。答えが来なかったのでなければ null。
 */
function answerMissing(path: string, error: HttpError): string | null {
  if (!error.timedOut) return null;
  return path === 'api/run'
    ? `${error.message}: the server may still start the run (it checks the file again when it has changed); ` +
        'whether it did shows in the session over the WebSocket'
    : `${error.message}: the server may have done it; the session over the WebSocket shows what happened`;
}

/** 断られた操作の理由と、そのときの実行（実行が変わったら、古い理由は出さない）。 */
interface ActionFailure {
  message: string;
  session: number | null;
  running: boolean;
}

/** 実行の操作と、その状態（GuiClient.run）。 */
export interface ClientRun {
  /** 開始を送って、応答を待っている。 */
  readonly starting: Readable<boolean>;
  /** 停止を送って、応答を待っている（どちらの停止か）。 */
  readonly stopping: Readable<StopAction | null>;
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting: Readable<boolean>;
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。実行が始まるか終わったら null。 */
  readonly error: Readable<string | null>;
  /**
   * 実行を始める（`POST api/run`）。応答の要約で解け、断られたら HttpError で reject する。サーバーは、開始の中で
   * 構成ファイルを照合し直すことがあるので、照合の上限の 2 倍（順番待ちと照合）に余裕を足した時間まで待つ。それでも
   * 答えがなければ、`timedOut` の HttpError で reject し、error に「答えが来なかった（始まったかは WebSocket の
   * 状態でわかる）」と出す。サーバーは始めたかもしれない。
   */
  start(request: RunRequest): Promise<SessionSummary | null>;
  /** 順に止める（`POST api/stop` `{force: false}`）。何度頼んでも同じ。 */
  stop(): Promise<SessionSummary | null>;
  /** 待たずに止める（`POST api/stop` `{force: true}`）。 */
  forceStop(): Promise<SessionSummary | null>;
}

/**
 * 実行の操作: 開始（`POST api/run`）、順に止める・待たずに止める（`POST api/stop`）と、そのための状態。
 *
 * - 操作の応答（いまの要約）は、通知を待たずにセッションの写しへ当てる（押した直後に古い表示が残らない）。
 * - 開始と停止は別々に送る。開始の応答を待っている間も、止めるよう頼める（サーバーは、ランチャーが起動した
 *   ところで届ける。順に止める停止は何度頼んでも同じなので、開始の応答の要約がまだ「頼んでいない」なら頼み直す）。
 * - 断られた理由（サーバーの英語の文）は error に出す。実行が始まるか終わったら出さない。
 * - start() と stop() の Promise は、応答の要約（読めなければ null）で解け、断られたら HttpError で reject する。
 *   答えが来なかった（`timedOut`）ときは、断られたのではない: 実行が始まったかは、セッションの写しでわかる。
 *   状態（starting など）は、Promise が解ける前に変わっている。
 */
export class RunControl implements ClientRun {
  private readonly startSending: Writable<boolean>;
  private readonly stopSending: Writable<StopAction | null>;
  private readonly stopAskedWhileStarting: Writable<boolean>;
  private readonly failure: Writable<ActionFailure | null>;

  readonly starting: Readable<boolean>;
  readonly stopping: Readable<StopAction | null>;
  readonly stopWhileStarting: Readable<boolean>;
  readonly error: Readable<string | null>;

  constructor(
    private readonly feed: SessionFeed,
    private readonly transport: HttpTransport,
    onError: ErrorSink = rethrowLater,
    /** 開始の応答を待つ時間（ミリ秒）。サーバーが開始の中で照合し直すことがあるので長い。 */
    private readonly startTimeoutMs: () => number = () => 0,
  ) {
    this.startSending = new Writable<boolean>(false, Object.is, onError);
    this.stopSending = new Writable<StopAction | null>(null, Object.is, onError);
    this.stopAskedWhileStarting = new Writable<boolean>(false, Object.is, onError);
    this.failure = new Writable<ActionFailure | null>(null, Object.is, onError);
    this.starting = this.startSending.readonly();
    this.stopping = this.stopSending.readonly();
    this.stopWhileStarting = this.stopAskedWhileStarting.readonly();
    this.error = derived(
      [this.failure, feed.session],
      () => {
        const failure = this.failure.get();
        const session = feed.session.get();
        if (!failure) return null;
        const same =
          failure.session === (session?.id ?? null) &&
          failure.running === (session?.launcher.running ?? false);
        return same ? failure.message : null;
      },
      Object.is,
      onError,
    );
  }

  /** 実行を始める。 */
  start(request: RunRequest): Promise<SessionSummary | null> {
    this.startSending.set(true);
    this.stopAskedWhileStarting.set(false);
    return this.send('api/run', request, this.startTimeoutMs() || undefined, () => {
      this.startSending.set(false);
      const asked = this.stopAskedWhileStarting.get();
      this.stopAskedWhileStarting.set(false);
      // 開始の応答の要約が、待っている間に頼んだ停止をまだ映していなければ、頼み直す（何度頼んでも同じ）
      const session = this.feed.session.get();
      if (asked && session?.launcher.running && session.stopRequests === 0) {
        this.stop().catch(() => undefined);
      }
    });
  }

  /** 順に止める（participant の終わりを待つ）。何度頼んでも、待たずに止めることにはならない。 */
  stop(): Promise<SessionSummary | null> {
    if (this.startSending.get()) this.stopAskedWhileStarting.set(true);
    this.stopSending.set('stop');
    return this.send('api/stop', { force: false }, undefined, () => this.stopSending.set(null));
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): Promise<SessionSummary | null> {
    this.stopSending.set('force');
    return this.send('api/stop', { force: true }, undefined, () => this.stopSending.set(null));
  }

  /** 送って、応答の要約を当てる。done は応答（失敗も）の後、Promise が解ける前に呼ぶ。 */
  private send(
    path: string,
    body: object,
    timeoutMs: number | undefined,
    done: () => void,
  ): Promise<SessionSummary | null> {
    this.failure.set(null);
    return new Promise((resolve, reject) => {
      this.transport({ method: 'POST', path, body, timeoutMs }, (outcome) => {
        if (!outcome.ok) {
          const session = this.feed.session.get();
          this.failure.set({
            message: answerMissing(path, outcome.error) ?? errorMessage(outcome.error),
            session: session?.id ?? null,
            running: session?.launcher.running ?? false,
          });
          done();
          reject(outcome.error);
          return;
        }
        let summary: SessionSummary | null = null;
        try {
          summary = readSessionResponse(outcome.body);
          this.feed.adopt(summary);
        } catch (error) {
          // 要約の芯が読めない応答は、知らせに出して、続く通知の要約に任せる
          this.feed.report(
            error instanceof ProtocolError ? `unreadable answer: ${error.message}` : String(error),
          );
        }
        done();
        resolve(summary);
      });
    });
  }
}
