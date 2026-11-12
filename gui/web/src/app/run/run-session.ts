import { HttpClient } from '@angular/common/http';
import { DestroyRef, Injectable, computed, inject, signal } from '@angular/core';
import { ProtocolError, RunRequest, errorMessage, readSessionResponse } from '../core/api';
import { ServerConnection } from '../core/session/server-connection';
import { RunOutcome, runOutcome } from '../core/session/session-state';

/** 停止の操作: 順に止める、待たずに止める。 */
export type StopAction = 'stop' | 'force';

/** 断られた操作の理由と、そのときの実行（実行が変わったら、古い理由は出さない）。 */
interface ActionFailure {
  message: string;
  session: number | null;
  running: boolean;
}

/**
 * 実行の画面の操作（開始、停止、待たずに止める）と、そのための状態。画面の providers に置き、画面と同じ寿命。
 *
 * - セッションの状態はアプリ全体の接続（ServerConnection）にあり、ここはそれを読む。
 * - 操作の応答（いまの要約）は、通知を待たずに接続の状態へ当てる（押した直後に古い表示が残らない）。
 * - 開始と停止は別々に送る。開始の応答を待っている間も、止めるよう頼める（サーバーは、ランチャーが起動した
 *   ところで届ける。順に止める停止は何度頼んでも同じなので、応答の要約がまだ「頼んでいない」なら頼み直す）。
 */
@Injectable()
export class RunSession {
  private readonly http = inject(HttpClient);
  private readonly server = inject(ServerConnection);

  private readonly startSending = signal(false);
  private readonly stopSending = signal<StopAction | null>(null);
  private readonly stopAskedWhileStarting = signal(false);
  private readonly actionFailure = signal<ActionFailure | null>(null);
  private readonly clock = signal(0);

  /** WebSocket の接続の状態。 */
  readonly connectionState = this.server.connectionState;
  /** 握手で断られた回数。 */
  readonly handshakeFailures = this.server.handshakeFailures;
  /** サーバーが「後でもう一度」で閉じた理由（つなぎ直している間だけ）。 */
  readonly tryAgainReason = this.server.tryAgainReason;
  /** サーバーと揃っている（つながっていて、再送を受け終えた）。 */
  readonly ready = this.server.ready;
  /** いまの（または最後の）セッション。 */
  readonly session = this.server.session;
  /** セッションにつながらない知らせ。 */
  readonly notices = this.server.notices;
  /** ランチャーが動いているか（結果が出た後も、終わるまでは動いている）。止められるかはこれで決める。 */
  readonly launcherRunning = computed(() => this.session()?.launcher.running ?? false);
  /** 実行がいまどうなっているか。セッションがなければ null。 */
  readonly outcome = computed<RunOutcome | null>(() => {
    const session = this.session();
    return session ? runOutcome(session) : null;
  });
  /** 開始を送って、応答を待っている。 */
  readonly starting = this.startSending.asReadonly();
  /** 停止を送って、応答を待っている（どちらの停止か）。 */
  readonly stopping = this.stopSending.asReadonly();
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = this.stopAskedWhileStarting.asReadonly();
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。実行が始まるか終わったら出さない。 */
  readonly actionError = computed(() => {
    const failure = this.actionFailure();
    const session = this.session();
    if (!failure) return null;
    const same =
      failure.session === (session?.id ?? null) &&
      failure.running === (session?.launcher.running ?? false);
    return same ? failure.message : null;
  });

  /**
   * 経過時間（ミリ秒）。事象の t_ms から、次の事象までは時計で進める。
   * ランチャーが終わったら、またはつながっていない間は進めない。
   */
  readonly elapsedMs = computed(() => {
    const session = this.session();
    if (!session) return null;
    if (!session.launcher.running || this.connectionState() !== 'open') return session.elapsedMs;
    this.clock();
    return session.elapsedMs + Math.max(0, Date.now() - this.server.lastEventAt());
  });

  constructor() {
    const timer = setInterval(() => this.clock.update((n) => n + 1), 1000);
    inject(DestroyRef).onDestroy(() => clearInterval(timer));
  }

  /** 実行を始める。 */
  start(request: RunRequest): void {
    this.startSending.set(true);
    this.stopAskedWhileStarting.set(false);
    this.send('api/run', request, () => {
      this.startSending.set(false);
      const asked = this.stopAskedWhileStarting();
      this.stopAskedWhileStarting.set(false);
      // 開始の応答の要約が、待っている間に頼んだ停止をまだ映していなければ、頼み直す（何度頼んでも同じ）
      const session = this.session();
      if (asked && session?.launcher.running && session.stopRequests === 0) this.stop();
    });
  }

  /** 順に止める（participant の終わりを待つ）。何度頼んでも、待たずに止めることにはならない。 */
  stop(): void {
    if (this.startSending()) this.stopAskedWhileStarting.set(true);
    this.stopSending.set('stop');
    this.send('api/stop', { force: false }, () => this.stopSending.set(null));
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): void {
    this.stopSending.set('force');
    this.send('api/stop', { force: true }, () => this.stopSending.set(null));
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.server.dismissNotice(id);
  }

  /** 送って、応答の要約を当てる。done は応答（失敗も）の後に呼ぶ。 */
  private send(url: string, body: object, done: () => void): void {
    this.actionFailure.set(null);
    this.http.post(url, body).subscribe({
      next: (response) => {
        try {
          this.server.adopt(readSessionResponse(response));
        } catch (error) {
          // 要約の芯が読めない応答は、知らせに出して、続く通知の要約に任せる
          this.server.report(
            error instanceof ProtocolError ? `unreadable answer: ${error.message}` : String(error),
          );
        }
        done();
      },
      error: (error: unknown) => {
        const session = this.session();
        this.actionFailure.set({
          message: errorMessage(error),
          session: session?.id ?? null,
          running: session?.launcher.running ?? false,
        });
        done();
      },
    });
  }
}
