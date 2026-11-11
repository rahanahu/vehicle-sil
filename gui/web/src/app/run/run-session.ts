import { HttpClient } from '@angular/common/http';
import { DestroyRef, Injectable, computed, inject, signal } from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import { EMPTY, bufferTime, defer, filter, interval, map, takeWhile } from 'rxjs';
import {
  AuthMessage,
  LauncherEvent,
  ProtocolError,
  RunRequest,
  ServerMessage,
  SessionSummary,
  errorMessage,
  readServerMessage,
} from '../core/api';
import { AuthToken } from '../core/auth/auth-token';
import { liveConnection } from '../core/streams/live-connection';
import {
  SessionView,
  addProblem,
  applyEvents,
  applySummary,
  isEnded,
  sessionFromSummary,
} from './run-state';

/** 届いた通知をまとめて画面へ渡す間隔（ミリ秒）。ログが速く流れても、画面の更新は毎秒この回数まで。 */
export const BATCH_MS = 100;

/** 画面に残すサーバーの知らせの数。 */
const MAX_NOTICES = 20;

/** セッションにつながらない知らせ（読めない通知など。英語の文はそのまま出す）。 */
export interface Notice {
  id: number;
  message: string;
}

/**
 * 実行のセッションを、サーバーの WebSocket から映す。実行の画面（RunPage）が providers に置き、画面と同じ寿命で動く。
 *
 * - 状態はサーバー（とランチャー）にあり、ここは届いた事象を当てた写しを持つ。
 *   つなぐたびに、最後に受け取った seq の次から再送を頼む（再読み込み、別のタブ、切断の後も同じ状態になる）。
 * - RxJS は WebSocket の流れ（再接続、まとめて渡す間引き）と経過時間の時計にだけ使い、外へは signal を出す。
 */
@Injectable()
export class RunSession {
  private readonly http = inject(HttpClient);
  private readonly auth = inject(AuthToken);

  private readonly current = signal<SessionView | null>(null);
  private readonly receivedAt = signal(0);
  private readonly noticeList = signal<readonly Notice[]>([]);
  private readonly pendingAction = signal<'start' | 'stop' | null>(null);
  private readonly actionFailure = signal<string | null>(null);
  private readonly clock = signal(0);
  private noticeId = 0;

  // 購読するのはこのサービスだけで、画面と同じ寿命。画面を離れたら（断られたら）すぐ切る
  private readonly connection = liveConnection<unknown>('ws', {
    greeting: () => [this.greeting()],
    lingerMs: 0,
  });

  /** WebSocket の接続の状態。 */
  readonly connectionState = this.connection.state;
  /** いまの（または最後の）セッション。サーバーにまだ何もなければ null。 */
  readonly session = this.current.asReadonly();
  /** セッションが動いているか（始まっていて、まだ終わっていない）。 */
  readonly active = computed(() => {
    const session = this.current();
    return session !== null && !isEnded(session);
  });
  /** セッションにつながらない知らせ。セッションの知らせ（seq の抜けなど）は session().problems。 */
  readonly notices = this.noticeList.asReadonly();
  /** 送っている最中の操作。 */
  readonly pending = this.pendingAction.asReadonly();
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。 */
  readonly actionError = this.actionFailure.asReadonly();

  /** 経過時間（ミリ秒）。事象の t_ms から、次の事象までは時計で進める。終わったら止まる。 */
  readonly elapsedMs = computed(() => {
    const session = this.current();
    if (!session) return null;
    if (isEnded(session)) return session.elapsedMs;
    this.clock();
    return session.elapsedMs + Math.max(0, Date.now() - this.receivedAt());
  });

  constructor() {
    const destroyRef = inject(DestroyRef);
    // トークンがなければつながない。断られたら、その知らせで流れを終え、つなぎ直さない（画面が開き直しを促す）
    defer(() => (this.auth.token() ? this.connection.messages : EMPTY))
      .pipe(
        map((message) => this.read(message)),
        filter((message) => message !== null),
        takeWhile(
          (message) => !(message.type === 'error' && message.code === 'unauthorized'),
          true,
        ),
        bufferTime(BATCH_MS),
        filter((batch) => batch.length > 0),
        takeUntilDestroyed(destroyRef),
      )
      .subscribe((batch) => this.apply(batch));
    interval(1000)
      .pipe(takeUntilDestroyed(destroyRef))
      .subscribe((n) => this.clock.set(n));
  }

  /** 実行を始める。 */
  start(request: RunRequest): void {
    this.send('start', 'api/run', request);
  }

  /** 止める。2 回目は待たずに止める。 */
  stop(): void {
    this.send('stop', 'api/stop', {});
  }

  /** 知らせを消す。 */
  dismissNotice(id: number): void {
    this.noticeList.update((list) => list.filter((notice) => notice.id !== id));
  }

  private send(action: 'start' | 'stop', url: string, body: object): void {
    this.pendingAction.set(action);
    this.actionFailure.set(null);
    this.http.post(url, body).subscribe({
      next: () => this.pendingAction.set(null),
      error: (error: unknown) => {
        this.pendingAction.set(null);
        this.actionFailure.set(errorMessage(error));
      },
    });
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
    const flush = () => {
      if (session && events.length > 0) session = applyEvents(session, events);
      events = [];
    };
    const adopt = (summary: SessionSummary) =>
      session?.id === summary.id
        ? applySummary(session, summary)
        : sessionFromSummary(summary, null);
    for (const message of batch) {
      if (message.type !== 'event') flush();
      switch (message.type) {
        case 'hello': {
          // 要約で作り直す。持っている分の続きを頼んだ再接続（from_seq つき）だけは、手元のログを残す
          const { session: summary, replay } = message;
          const resumed = summary && session?.id === summary.id && replay?.from_seq != null;
          session = !summary
            ? null
            : resumed && session
              ? applySummary(session, summary)
              : sessionFromSummary(summary, replay);
          break;
        }
        case 'session':
          session = adopt(message.session);
          break;
        case 'event':
          if (session?.id === message.session) events.push(message.event);
          break;
        case 'error':
          if (message.code === 'unauthorized') {
            this.auth.markRejected();
          } else if (session && message.session === session.id) {
            session = addProblem(session, { code: message.code, message: message.message });
          } else {
            this.notify(message.message);
          }
          break;
      }
    }
    flush();
    this.current.set(session);
    this.receivedAt.set(Date.now());
  }

  private notify(message: string): void {
    const notice = { id: ++this.noticeId, message };
    this.noticeList.update((list) => [...list, notice].slice(-MAX_NOTICES));
  }
}
