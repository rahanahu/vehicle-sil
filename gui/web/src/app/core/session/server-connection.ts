import { DestroyRef, Injectable, inject } from '@angular/core';
import { ServerMessage } from '@vehicle-sil/gui-client';
import { Observable, share } from 'rxjs';
import { AuthToken } from '../auth/auth-token';
import { GUI_CLIENT, readableSignal } from './gui-client';

/**
 * GUI サーバーとの接続（WebSocket）とセッションの写しの、Angular の包み。アプリ全体で 1 つ（どの画面にいても、
 * つながったまま）。
 *
 * - 中身はクライアントのライブラリ（`@vehicle-sil/gui-client` の Connection と SessionFeed）。ここは、その読める値を
 *   signal にするだけ。つなぐ、つなぎ直す、要約と事象を当てる、はライブラリがする。
 * - 画面はここの signal を読む。値のように速い流れを自分で当てたい画面には、まとめた通知の流れ（batches）を出す。
 */
@Injectable({ providedIn: 'root' })
export class ServerConnection {
  private readonly client = inject(GUI_CLIENT);
  private readonly auth = inject(AuthToken);
  private readonly destroyRef = inject(DestroyRef);

  /** WebSocket の接続の状態。 */
  readonly connectionState = readableSignal(this.client.connection.state);
  /** 握手で断られた（開く前に閉じた）回数。増えたら、画面は api/app を呼んで理由を知る。 */
  readonly handshakeFailures = readableSignal(this.client.connection.failures);
  /**
   * サーバーが接続を「後でもう一度」（1013）で閉じた理由の文（`too-many-connections`、`too-many-unauthenticated`）。
   * 間を置いてつなぎ直していて、つながり直したら null。
   */
  readonly tryAgainReason = readableSignal(this.client.connection.tryAgainReason);
  /**
   * セッションの写しがサーバーと揃っている（つながっていて、hello と再送を受け終えた）。揃っていない間は、古い表示を
   * 見て操作しないよう、開始と停止を押せなくする。
   */
  readonly ready = readableSignal(this.client.session.ready);
  /** いまの（または最後の）セッション。サーバーにまだ何もなければ null。 */
  readonly session = readableSignal(this.client.session.session);
  /** 最後に事象を当てた時刻（Date.now()）。経過時間を時計で進める起点。 */
  readonly lastEventAt = readableSignal(this.client.session.lastEventAt);
  /** セッションにつながらない知らせ。セッションの知らせ（seq の抜けなど）は session().problems。 */
  readonly notices = readableSignal(this.client.session.notices);
  /** サーバーの口の版がこの画面と違うので、つながない（そのわけの英語の文）。違わなければ null。 */
  readonly incompatible = readableSignal(this.client.session.incompatible);

  /**
   * 読めた通知を BATCH_MS（`@vehicle-sil/gui-client`）ごとにまとめた流れ（共有）。セッションの写しに当てた後に届く。
   * 別の画面が自分の signal に当てるときに購読する（購読しても接続は増えない）。
   */
  readonly batches: Observable<readonly ServerMessage[]> = new Observable<readonly ServerMessage[]>(
    (subscriber) => this.client.session.onMessages((batch) => subscriber.next(batch)),
  ).pipe(share());

  constructor() {
    // WebSocket で断られたら、トークンが断られたことにする（画面が開き直しを促す）
    const stop = this.client.session.rejected.subscribe((rejected) => {
      if (rejected) this.auth.markRejected();
    });
    this.destroyRef.onDestroy(stop);
    this.client.connect();
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.client.session.dismissNotice(id);
  }
}
