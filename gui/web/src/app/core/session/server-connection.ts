import { DestroyRef, Injectable, computed, inject, signal } from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import { EMPTY, Observable, bufferTime, defer, filter, map, share, takeWhile } from 'rxjs';
import {
  AuthMessage,
  LauncherEvent,
  ProtocolError,
  ServerMessage,
  SessionSummary,
  readServerMessage,
} from '../api';
import { AuthToken } from '../auth/auth-token';
import { liveConnection } from '../streams/live-connection';
import {
  SessionView,
  addDropped,
  addProblem,
  applyEvents,
  applySummary,
  sessionFromSummary,
} from './session-state';

/** 届いた通知をまとめて画面へ渡す間隔（ミリ秒）。ログが速く流れても、画面の更新は毎秒この回数まで。 */
export const BATCH_MS = 100;

/** サーバーが「後でもう一度」で閉じたときの閉じるコード。つなぎ直す（トークンを断ったのではない）。 */
export const TRY_AGAIN_LATER = 1013;

/** 画面に残す、セッションにつながらない知らせの種類の数。 */
export const MAX_NOTICES = 20;

/** セッションにつながらない知らせ（読めない通知など。英語の文はそのまま出す）。同じ文は数をまとめる。 */
export interface Notice {
  id: number;
  message: string;
  /** 同じ文が届いた回数。 */
  count: number;
}

/**
 * GUI サーバーとの接続（WebSocket）。アプリ全体で 1 つ（どの画面にいても、つながったまま）。
 *
 * - つなぐたびに、トークンと「持っているセッションのどこから再送してほしいか」を送る（再読み込み、別のタブ、切断の後も同じ状態）。
 * - hello の要約でセッションの状態を作り直し（要約が正）、続く事象（再送のログと、新しい事象）を当てる。
 * - 画面はここの signal を読む。値のように速い流れを自分で当てたい画面には、まとめた通知の流れ（batches）を出す。
 * - RxJS は WebSocket の流れ（再接続、まとめて渡す間引き）にだけ使う。
 */
@Injectable({ providedIn: 'root' })
export class ServerConnection {
  private readonly auth = inject(AuthToken);

  private readonly current = signal<SessionView | null>(null);
  private readonly synced = signal(false);
  private readonly receivedAt = signal(0);
  private readonly noticeList = signal<readonly Notice[]>([]);
  private noticeId = 0;

  // 購読するのはこのサービスだけで、アプリと同じ寿命。断られたら、すぐ切る（つなぎ直さない）
  private readonly connection = liveConnection<unknown>('ws', {
    lingerMs: 0,
    greeting: () => {
      // つなぎ直すたびに、hello と再送を受け終えるまでは「揃っていない」
      this.synced.set(false);
      return [this.greeting()];
    },
  });

  /** WebSocket の接続の状態。 */
  readonly connectionState = this.connection.state;
  /** 握手で断られた（開く前に閉じた）回数。増えたら、画面は api/app を呼んで理由を知る。 */
  readonly handshakeFailures = this.connection.failures;
  /**
   * サーバーが接続を「後でもう一度」（1013）で閉じた理由の文（`too-many-connections`、`too-many-unauthenticated`）。
   * 間を置いてつなぎ直していて、つながり直したら null。
   */
  readonly tryAgainReason = computed(() => {
    const close = this.connection.lastClose();
    return close?.code === TRY_AGAIN_LATER ? close.reason || 'try-again-later' : null;
  });
  /**
   * つながっていて、hello と再送を受け終えた（画面の状態がサーバーと揃っている）。
   * 揃っていない間は、古い表示を見て操作しないよう、開始と停止を押せなくする。
   */
  readonly ready = computed(() => this.connectionState() === 'open' && this.synced());
  /** いまの（または最後の）セッション。サーバーにまだ何もなければ null。 */
  readonly session = this.current.asReadonly();
  /** 最後に事象を当てた時刻（Date.now()）。経過時間を時計で進める起点。 */
  readonly lastEventAt = this.receivedAt.asReadonly();
  /** セッションにつながらない知らせ。セッションの知らせ（seq の抜けなど）は session().problems。 */
  readonly notices = this.noticeList.asReadonly();

  /**
   * 読み取った通知を BATCH_MS ごとにまとめた流れ（共有）。このサービス自身がセッションの状態に当てる。
   * 別の画面が自分の signal に当てるときに購読する（購読しても接続は増えない）。
   */
  readonly batches: Observable<readonly ServerMessage[]>;

  constructor() {
    // トークンがなければつながない。断られたら、その知らせで流れを終え、つなぎ直さない（画面が開き直しを促す）
    this.batches = defer(() => (this.auth.token() ? this.connection.messages : EMPTY)).pipe(
      map((message) => this.read(message)),
      filter((message) => message !== null),
      takeWhile((message) => !(message.type === 'error' && message.code === 'unauthorized'), true),
      bufferTime(BATCH_MS),
      filter((batch) => batch.length > 0),
      share(),
    );
    this.batches
      .pipe(takeUntilDestroyed(inject(DestroyRef)))
      .subscribe((batch) => this.apply(batch));
  }

  /** HTTP の応答の要約（開始、停止）を、通知を待たずに当てる。 */
  adopt(summary: SessionSummary | null): void {
    if (!summary) return;
    const session = this.current();
    // 前の実行の要約（始まりかけの実行への停止の応答など）で、新しい実行を戻さない
    if (session && summary.id < session.id) return;
    // 応答と通知の順は決まらないので、手元の事象より新しい要約のときだけ段階などを取る
    this.current.set(
      session?.id === summary.id ? applySummary(session, summary) : sessionFromSummary(summary),
    );
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.noticeList.update((list) => list.filter((notice) => notice.id !== id));
  }

  /** つなぐたびに最初に送るメッセージ: トークンと、持っているセッションのどこから再送してほしいか。 */
  private greeting(): AuthMessage {
    const session = this.current();
    const auth: AuthMessage = { type: 'auth', token: this.auth.token() ?? '' };
    return session ? { ...auth, session: session.id, from_seq: session.lastSeq + 1 } : auth;
  }

  private read(message: unknown): ServerMessage | null {
    try {
      return readServerMessage(message);
    } catch (error) {
      this.notify(
        error instanceof ProtocolError ? `unreadable message: ${error.message}` : String(error),
      );
      return null;
    }
  }

  private apply(batch: readonly ServerMessage[]): void {
    let session = this.current();
    let events: LauncherEvent[] = [];
    let applied = false;
    const flush = () => {
      if (session && events.length > 0) {
        session = applyEvents(session, events);
        applied = true;
      }
      events = [];
    };
    for (const message of batch) {
      if (message.type !== 'event') flush();
      switch (message.type) {
        case 'hello': {
          // 要約で作り直す。持っている分の続きを頼んだ再接続（from_seq つき）だけは、手元のログを残す
          this.synced.set(false);
          const { session: summary, replay } = message;
          const resumed = summary && session?.id === summary.id && replay?.from_seq != null;
          session = !summary
            ? null
            : resumed && session
              ? applySummary(session, summary, true)
              : sessionFromSummary(summary);
          break;
        }
        case 'dropped': {
          if (session && message.session === session.id) session = addDropped(session, message);
          break;
        }
        case 'synced':
          this.synced.set(true);
          break;
        case 'session':
          // 要約が正: 事象と同じ順で届くので、届いた時点のサーバーの状態として置き換える（ログは残す）
          session =
            session?.id === message.session.id
              ? applySummary(session, message.session, true)
              : sessionFromSummary(message.session);
          break;
        case 'event':
          if (session?.id === message.session) events.push(message.event);
          break;
        case 'error':
          if (message.code === 'unauthorized') {
            this.auth.markRejected();
          } else if (session && message.session === session.id) {
            const { code, message: text, skipped } = message;
            session = addProblem(
              session,
              skipped === undefined ? { code, message: text } : { code, message: text, skipped },
            );
          } else {
            this.notify(message.message);
          }
          break;
        case 'resync':
          // すぐ後に hello から送り直される。それまでは揃っていない
          this.synced.set(false);
          break;
      }
    }
    flush();
    this.current.set(session);
    if (applied) this.receivedAt.set(Date.now());
  }

  private notify(message: string): void {
    this.noticeList.update((list) => {
      const same = list.find((notice) => notice.message === message);
      if (same) {
        return [...list.filter((n) => n !== same), { ...same, count: same.count + 1 }];
      }
      return [...list, { id: ++this.noticeId, message, count: 1 }].slice(-MAX_NOTICES);
    });
  }
}
