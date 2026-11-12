import { HttpClient } from '@angular/common/http';
import { DestroyRef, Injectable, computed, inject, signal } from '@angular/core';
import { RunRequest, errorMessage, readSessionResponse } from '../core/api';
import { ServerConnection } from '../core/session/server-connection';
import { RunOutcome, runOutcome } from '../core/session/session-state';

/** 送っている最中の操作。 */
export type RunAction = 'start' | 'stop' | 'force';

/**
 * 実行の画面の操作（開始、停止、待たずに止める）と、そのための状態。画面の providers に置き、画面と同じ寿命。
 *
 * - セッションの状態はアプリ全体の接続（ServerConnection）にあり、ここはそれを読む。
 * - 操作の応答（いまの要約）は、通知を待たずに接続の状態へ当てる（押した直後に古い表示が残らない）。
 */
@Injectable()
export class RunSession {
  private readonly http = inject(HttpClient);
  private readonly server = inject(ServerConnection);

  private readonly pendingAction = signal<RunAction | null>(null);
  private readonly actionFailure = signal<string | null>(null);
  private readonly clock = signal(0);

  /** WebSocket の接続の状態。 */
  readonly connectionState = this.server.connectionState;
  /** 握手で断られた回数。 */
  readonly handshakeFailures = this.server.handshakeFailures;
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
  /** 送っている最中の操作。 */
  readonly pending = this.pendingAction.asReadonly();
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。 */
  readonly actionError = this.actionFailure.asReadonly();

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
    this.send('start', 'api/run', request);
  }

  /** 順に止める（participant の終わりを待つ）。何度頼んでも、待たずに止めることにはならない。 */
  stop(): void {
    this.send('stop', 'api/stop', { force: false });
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): void {
    this.send('force', 'api/stop', { force: true });
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.server.dismissNotice(id);
  }

  private send(action: RunAction, url: string, body: object): void {
    this.pendingAction.set(action);
    this.actionFailure.set(null);
    this.http.post(url, body).subscribe({
      next: (response) => {
        this.pendingAction.set(null);
        try {
          this.server.adopt(readSessionResponse(response));
        } catch {
          // 読めない応答は、続く通知の要約に任せる
        }
      },
      error: (error: unknown) => {
        this.pendingAction.set(null);
        this.actionFailure.set(errorMessage(error));
      },
    });
  }
}
