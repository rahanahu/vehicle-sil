import { DestroyRef, Injectable, computed, inject, signal } from '@angular/core';
import { RunOutcome, RunRequest, runOutcome } from '@vehicle-sil/gui-client';
import { RunCommands } from '../core/session/run-commands';
import { ServerConnection } from '../core/session/server-connection';

export type { StopAction } from '@vehicle-sil/gui-client';

/**
 * 実行の画面の操作（開始、停止、待たずに止める）と、そのための状態。画面の providers に置き、画面と同じ寿命。
 *
 * - セッションの状態はアプリ全体の接続（ServerConnection）に、操作とその状態は RunCommands にあり、ここはそれを
 *   まとめて読む（中身はクライアントのライブラリ）。
 * - 操作の応答（いまの要約）は、通知を待たずにセッションの写しへ当たる（押した直後に古い表示が残らない）。
 * - 開始と停止は別々に送る。開始の応答を待っている間も、止めるよう頼める。
 */
@Injectable()
export class RunSession {
  private readonly server = inject(ServerConnection);
  private readonly commands = inject(RunCommands);

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
  /** サーバーの口の版がこの画面と違うので、つながない（そのわけの英語の文）。違わなければ null。 */
  readonly incompatible = this.server.incompatible;
  /** ランチャーが動いているか（結果が出た後も、終わるまでは動いている）。止められるかはこれで決める。 */
  readonly launcherRunning = computed(() => this.session()?.launcher.running ?? false);
  /** 実行がいまどうなっているか。セッションがなければ null。 */
  readonly outcome = computed<RunOutcome | null>(() => {
    const session = this.session();
    return session ? runOutcome(session) : null;
  });
  /** 開始を送って、応答を待っている。 */
  readonly starting = this.commands.starting;
  /** 停止を送って、応答を待っている（どちらの停止か）。 */
  readonly stopping = this.commands.stopping;
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = this.commands.stopWhileStarting;
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。実行が始まるか終わったら出さない。 */
  readonly actionError = this.commands.error;

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
    this.commands.start(request);
  }

  /** 順に止める（participant の終わりを待つ）。何度頼んでも、待たずに止めることにはならない。 */
  stop(): void {
    this.commands.stop();
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): void {
    this.commands.forceStop();
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.server.dismissNotice(id);
  }
}
