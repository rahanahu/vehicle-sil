import { Connection } from './connection';
import {
  AuthMessage,
  LauncherEvent,
  PROTOCOL_VERSION,
  ProtocolError,
  ServerMessage,
  SessionSummary,
  readServerMessage,
} from './protocol';
import {
  SessionView,
  addDropped,
  addProblem,
  applyEvents,
  applySummary,
  sessionFromSummary,
} from './session-state';
import { ErrorSink, Readable, Writable, callEach, derived, rethrowLater } from './values';

/** 残す、セッションにつながらない知らせの種類の数。 */
export const MAX_NOTICES = 20;

/** seq が前の事象より後でない事象を当てなかったときの知らせ（英語。ほかの知らせと同じく、そのまま出す）。 */
export const STALE_EVENTS_NOTICE =
  'events came with a seq that is not after the last one, and were not shown ' +
  '(the launcher wrote them out of order, or the server sent them again)';

/** セッションにつながらない知らせ（読めない通知など。英語の文はそのまま出す）。同じ文は数をまとめる。 */
export interface Notice {
  id: number;
  message: string;
  /** 同じ文が届いた回数。 */
  count: number;
}

/** セッションの写しの、読むための口（GuiClient.session）。 */
export interface ClientSession {
  /** いまの（または最後の）セッション。サーバーにまだ何もなければ null。 */
  readonly session: Readable<SessionView | null>;
  /**
   * セッションの写しがサーバーと揃っている: つながっていて、hello と再送を受け終えた。揃っていない間は、古い表示を
   * 見て操作しないよう、開始と停止を押せなくする。ほかの機能（値など）は、それぞれの ready を持つ。
   */
  readonly ready: Readable<boolean>;
  /** 最後に事象を当てた時刻（Date.now()）。経過時間を時計で進める起点。 */
  readonly lastEventAt: Readable<number>;
  /** セッションにつながらない知らせ。セッションの知らせ（seq の抜けなど）は session の problems。 */
  readonly notices: Readable<readonly Notice[]>;
  /**
   * サーバーがトークンを断った（WebSocket の unauthorized）。接続はもうつなぎ直さない。一度立つと戻らない（別の
   * トークンで使うときは、クライアントを作り直す）。
   */
  readonly rejected: Readable<boolean>;
  /**
   * サーバーの口の版が、このライブラリの読む版（PROTOCOL_VERSION）と違う。そのわけの文（英語）。接続はもう
   * つなぎ直さない（待っても直らない）。一度立つと戻らない。違わなければ null。
   */
  readonly incompatible: Readable<string | null>;
  /** 読めた通知を、状態に当てた後にまとまりごとに受ける。返す関数で購読をやめる。 */
  onMessages(listener: (batch: readonly ServerMessage[]) => void): () => void;
  /** 利用者が気づいたこと（読めない応答など）を、知らせに出す。同じ文は数をまとめる。 */
  report(message: string, count?: number): void;
  /** 知らせを消す。 */
  dismissNotice(id: number): void;
}

/**
 * セッション（ランチャーの 1 回の run）の写し。接続（Connection）の通知を読み、サーバーの要約と事象から状態を作る。
 *
 * - hello の要約で状態を作り直し（要約が正）、続く事象（再送のログと、新しい事象）を当てる。
 * - つなぐたびに、持っているセッションのどこから再送してほしいかを、認証のメッセージに入れる（resumeFields）。
 * - 要約の芯が読めない hello か session を受けたら、揃っていないことにして、接続に作り直しを頼む（hello から受け直す）。
 * - 知らない通知と知らない事象は、状態に当てずに通す（onMessages には読めたものだけ、接続の onBatch にはすべて）。
 * - 接続への頼みごと（断られたら切る、作り直す）は、購読者へ知らせる前に済ませる（購読者が投げても進む）。
 */
export class SessionFeed implements ClientSession {
  private readonly current: Writable<SessionView | null>;
  private readonly synced: Writable<boolean>;
  private readonly receivedAt: Writable<number>;
  private readonly noticeList: Writable<readonly Notice[]>;
  private readonly rejectedValue: Writable<boolean>;
  private readonly incompatibleValue: Writable<string | null>;
  private readonly messageListeners = new Set<(batch: readonly ServerMessage[]) => void>();
  private noticeId = 0;
  private stoppedFlag = false;

  readonly session: Readable<SessionView | null>;
  readonly ready: Readable<boolean>;
  readonly lastEventAt: Readable<number>;
  readonly notices: Readable<readonly Notice[]>;
  readonly rejected: Readable<boolean>;
  readonly incompatible: Readable<string | null>;

  constructor(
    private readonly connection: Connection,
    private readonly onError: ErrorSink = rethrowLater,
  ) {
    this.current = new Writable<SessionView | null>(null, Object.is, onError);
    this.synced = new Writable<boolean>(false, Object.is, onError);
    this.receivedAt = new Writable<number>(0, Object.is, onError);
    this.noticeList = new Writable<readonly Notice[]>([], Object.is, onError);
    this.rejectedValue = new Writable<boolean>(false, Object.is, onError);
    this.incompatibleValue = new Writable<string | null>(null, Object.is, onError);
    this.session = this.current.readonly();
    this.lastEventAt = this.receivedAt.readonly();
    this.notices = this.noticeList.readonly();
    this.rejected = this.rejectedValue.readonly();
    this.incompatible = this.incompatibleValue.readonly();
    this.ready = derived(
      [connection.state, this.synced],
      () => connection.state.get() === 'open' && this.synced.get(),
      Object.is,
      onError,
    );
    connection.onBatch((batch) => this.receive(batch));
    connection.onUnreadable((text) =>
      this.notify(`unreadable message: not JSON (connecting again): ${text.slice(0, 80)}`),
    );
  }

  /** 断られたか、版が違って、もうつながない（読める値を立てる前、接続を切る前に立つ）。 */
  get stopped(): boolean {
    return this.stoppedFlag;
  }

  /**
   * 認証のメッセージに足す欄: 持っているセッションのどこから再送してほしいか。接続がつなぐたびに呼ぶ（呼ぶと、
   * hello と再送を受け終えるまで「揃っていない」になる）。ライブラリの中だけで使う。
   */
  resumeFields(): Pick<AuthMessage, 'session' | 'from_seq'> {
    this.synced.set(false);
    const session = this.current.get();
    return session ? { session: session.id, from_seq: session.lastSeq + 1 } : {};
  }

  onMessages(listener: (batch: readonly ServerMessage[]) => void): () => void {
    this.messageListeners.add(listener);
    return () => {
      this.messageListeners.delete(listener);
    };
  }

  /**
   * HTTP の応答の要約を、通知を待たずに当てる（開始、停止、fetchSession() の応答。ライブラリの中で使う）。
   * WebSocket がつながっていれば、同じ実行では、ランチャーと停止の依頼だけを取る（ほかは通知で届く）。前の実行の
   * 要約では、新しい実行を戻さない。
   */
  adopt(summary: SessionSummary | null): void {
    if (!summary) return;
    const session = this.current.get();
    // 前の実行の要約（始まりかけの実行への停止の応答など）で、新しい実行を戻さない
    if (session && summary.id < session.id) return;
    const live = this.connection.state.get() === 'open';
    this.current.set(
      session?.id === summary.id
        ? applySummary(session, summary, false, live)
        : sessionFromSummary(summary),
    );
  }

  report(message: string, count = 1): void {
    this.notify(message, count);
  }

  dismissNotice(id: number): void {
    this.noticeList.update((list) => list.filter((notice) => notice.id !== id));
  }

  /** 接続から届いたまとまりを読んで当てる。 */
  private receive(raw: readonly unknown[]): void {
    const batch: ServerMessage[] = [];
    let stop: 'unauthorized' | 'incompatible' | 'lost' | null = null;
    let incompatible: string | null = null;
    for (const value of raw) {
      let message: ServerMessage | null;
      try {
        message = readServerMessage(value);
      } catch (error) {
        this.notify(
          error instanceof ProtocolError ? `unreadable message: ${error.message}` : String(error),
        );
        const type = (value as { type?: unknown } | null)?.type;
        if (
          type === 'hello' &&
          error instanceof ProtocolError &&
          error.path === 'message.vehicle_sil_gui'
        ) {
          // 版が違う: 待っても直らないので、つなぎ直さない
          const version = JSON.stringify((value as { vehicle_sil_gui?: unknown }).vehicle_sil_gui);
          incompatible =
            `the server speaks version ${version} of the GUI protocol (vehicle_sil_gui) and this client ` +
            `reads version ${PROTOCOL_VERSION}: use a client made for the server's version`;
          stop = 'incompatible';
          break;
        }
        if (type === 'hello' || type === 'session') {
          // 状態を作り直せないので、揃っていないことにして、hello から受け直す（残りは捨てる）
          stop = 'lost';
          break;
        }
        continue;
      }
      if (message === null) continue;
      batch.push(message);
      if (message.type === 'error' && message.code === 'unauthorized') {
        // 断られた: 続きは受けず、つなぎ直さない（画面が開き直しを促す）
        stop = 'unauthorized';
        break;
      }
    }
    // 接続への頼みごとを、購読者へ知らせる前に済ませる。止める印は切る前に立てる（切った知らせの中で connect() を
    // 呼ばれても、つながない）
    if (stop === 'unauthorized' || stop === 'incompatible') {
      this.stoppedFlag = true;
      this.connection.close();
    }
    if (stop === 'lost') this.connection.restart();
    this.apply(batch);
    if (stop === 'lost' || stop === 'incompatible') this.synced.set(false);
    if (stop === 'unauthorized') this.rejectedValue.set(true);
    if (incompatible !== null) this.incompatibleValue.set(incompatible);
  }

  private apply(batch: readonly ServerMessage[]): void {
    if (batch.length === 0) return;
    const before = this.current.get();
    let session = before;
    let events: LauncherEvent[] = [];
    let applied = false;
    let synced = this.synced.get();
    // hello から synced までの再送は、つなぎ直す前に受けていた分と重なることがある（数えない）
    let replaying = !synced;
    const flush = () => {
      if (session && events.length > 0) {
        session = applyEvents(session, events, !replaying);
        applied = true;
      }
      events = [];
    };
    for (const message of batch) {
      if (message.type !== 'event') flush();
      switch (message.type) {
        case 'hello': {
          // 要約で作り直す。持っている分の続きを頼んだ再接続（from_seq つき）だけは、手元のログを残す
          synced = false;
          replaying = true;
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
          synced = true;
          replaying = false;
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
            // rejected で知らせる（receive が接続を切る）
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
          synced = false;
          replaying = true;
          break;
      }
    }
    flush();
    const stale =
      (session?.staleEvents ?? 0) - (before?.id === session?.id ? (before?.staleEvents ?? 0) : 0);
    this.current.set(session);
    this.synced.set(synced);
    if (applied) this.receivedAt.set(Date.now());
    if (stale > 0) this.notify(STALE_EVENTS_NOTICE, stale);
    callEach(this.messageListeners, [batch], this.onError);
  }

  private notify(message: string, count = 1): void {
    this.noticeList.update((list) => {
      const same = list.find((notice) => notice.message === message);
      if (same) {
        return [...list.filter((n) => n !== same), { ...same, count: same.count + count }];
      }
      return [...list, { id: ++this.noticeId, message, count }].slice(-MAX_NOTICES);
    });
  }
}
