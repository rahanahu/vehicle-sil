import { Connection } from './connection';
import {
  AuthMessage,
  LauncherEvent,
  PROTOCOL_VERSION,
  ProtocolError,
  ServerAllowed,
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

/** How many kinds of notices not tied to a session are kept. */
export const MAX_NOTICES = 20;

/**
 * Notice for an event not applied because its seq was not after the previous event's (English, shown as is like the
 * other notices).
 */
export const STALE_EVENTS_NOTICE =
  'events came with a seq that is not after the last one, and were not shown ' +
  '(the launcher wrote them out of order, or the server sent them again)';

/**
 * Notice not tied to a session (an unreadable notification and so on; the English text is shown as is). Identical texts
 * are counted together.
 */
export interface Notice {
  id: number;
  message: string;
  /** How many times the same text arrived. */
  count: number;
}

/** Read side of the session copy (GuiClient.session). */
export interface ClientSession {
  /** The current (or last) session. null while the server has none yet. */
  readonly session: Readable<SessionView | null>;
  /** Identity of the session suitable for a targeted action. */
  currentTarget(): { session: number; instance: string } | null;
  /** The last authenticated server instance, even when there is no session. */
  currentInstance(): string | null;
  /**
   * The session copy matches the server: connected, and hello and the resent events have all arrived. Until then,
   * Start and Stop are disabled so that nobody acts on a stale display. Other features (values and so on) have their
   * own ready.
   */
  readonly ready: Readable<boolean>;
  /**
   * What the server accepts now (start, stop, force and why not). null until it says (hello), and from a server that
   * does not say. What the launcher accepts is `launcherState` of the session.
   */
  readonly allowed: Readable<ServerAllowed | null>;
  /** When an event was last applied (Date.now()). The starting point for advancing elapsed time with the clock. */
  readonly lastEventAt: Readable<number>;
  /**
   * Notices not tied to a session. Notices about the session (a gap in seq and so on) are in the problems of session.
   */
  readonly notices: Readable<readonly Notice[]>;
  /**
   * The server refused the token (WebSocket unauthorized). The connection no longer reconnects. Once set, it stays
   * set (to use another token, create a new client).
   */
  readonly rejected: Readable<boolean>;
  /**
   * The text (English) saying why the server API's version differs from the version this library reads
   * (PROTOCOL_VERSION). The connection no longer reconnects (waiting will not fix it). Once set, it stays set. null if
   * the versions match.
   */
  readonly incompatible: Readable<string | null>;
  /**
   * Receives the readable notifications, one batch at a time, after they are applied to the state. Call the returned
   * function to unsubscribe.
   */
  onMessages(listener: (batch: readonly ServerMessage[]) => void): () => void;
  /**
   * Adds something the user noticed (an unreadable answer and so on) to the notices. Identical texts are counted
   * together.
   */
  report(message: string, count?: number): void;
  /** Removes a notice. */
  dismissNotice(id: number): void;
}

interface Adoption {
  revision: number;
  instance: string | null;
  session: number | null;
}

/**
 * Copy of a session (one run of the launcher). Reads the notifications of the connection (Connection) and builds the
 * state from the server's summary and events.
 *
 * - Rebuilds the state from the summary in hello (the summary is authoritative), then applies the events that follow
 *   (the resent log and new events).
 * - On every connect, puts into the authentication message where in the session it holds the server should resend
 *   from (resumeFields).
 * - On a hello or session whose summary core is unreadable, marks itself not in sync and asks the connection to
 *   restart (receive again from hello).
 * - Passes unknown notifications and unknown events through without applying them (onMessages gets only the readable
 *   ones, the connection's onBatch gets all of them).
 * - Requests to the connection (disconnect after a refusal, restart) are made before subscribers are notified (so
 *   they happen even if a subscriber throws).
 */
export class SessionFeed implements ClientSession {
  private readonly rawListeners = new Set<
    (batch: readonly unknown[], current: () => boolean, snapshotVersion: number) => void
  >();
  private snapshotVersion: () => number = () => 0;
  private instanceId: string | null = null;
  private adoptionRevision = 0;
  private instanceListener: (instance: string, current: () => boolean) => void = () => undefined;
  private stopTarget: { session: number; instance: string } | null = null;
  private readonly current: Writable<SessionView | null>;
  private readonly synced: Writable<boolean>;
  private readonly allowedValue: Writable<ServerAllowed | null>;
  private readonly receivedAt: Writable<number>;
  private readonly noticeList: Writable<readonly Notice[]>;
  private readonly rejectedValue: Writable<boolean>;
  private readonly incompatibleValue: Writable<string | null>;
  private readonly messageListeners = new Set<(batch: readonly ServerMessage[]) => void>();
  private noticeId = 0;
  private stoppedFlag = false;

  readonly session: Readable<SessionView | null>;
  readonly ready: Readable<boolean>;
  readonly allowed: Readable<ServerAllowed | null>;
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
    this.allowedValue = new Writable<ServerAllowed | null>(null, Object.is, onError);
    this.allowed = this.allowedValue.readonly();
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

  /** Refused, or the versions differ: never connect again (set before the readable values and before disconnecting). */
  get stopped(): boolean {
    return this.stoppedFlag;
  }

  /**
   * Fields to add to the authentication message: where in the session it holds the server should resend from. Called
   * on every connect (after the call, the copy is not in sync until hello and the resent events have arrived). Used
   * only inside the library.
   */
  resumeFields(): Pick<AuthMessage, 'session' | 'from_seq'> {
    this.synced.set(false);
    // A replacement server may reuse a session ID, and older servers ignore instance guards.
    // Request the retained replay from its beginning rather than send a cursor for a different run.
    return {};
  }

  onMessages(listener: (batch: readonly ServerMessage[]) => void): () => void {
    this.messageListeners.add(listener);
    return () => {
      this.messageListeners.delete(listener);
    };
  }

  /**
   * Applies the summary in an HTTP answer without waiting for the notification (the answers to start, stop and
   * fetchSession(); used inside the library). While the WebSocket is connected, takes only the launcher and the stop
   * request for the same run (the rest arrives in notifications). A summary of an earlier run never brings back an old
   * run over a new one.
   */
  onRawMessages(
    listener: (batch: readonly unknown[], current: () => boolean, snapshotVersion: number) => void,
  ): () => void {
    this.rawListeners.add(listener);
    return () => this.rawListeners.delete(listener);
  }

  onSnapshotVersion(read: () => number): void {
    this.snapshotVersion = read;
  }

  /** Called before a new server's session can be published. */
  onInstance(listener: (instance: string, current: () => boolean) => void): void {
    this.instanceListener = listener;
  }

  /** Instance associated with the current session, or null if its origin is not known. */
  currentInstance(): string | null {
    return this.instanceId;
  }

  currentTarget(): { session: number; instance: string } | null {
    return this.stopTarget ? { ...this.stopTarget } : null;
  }

  adopt(
    summary: SessionSummary | null,
    instance?: string | null,
    resyncFrom?: string | null,
  ): Adoption | null {
    const switching =
      instance !== undefined && this.instanceId !== null && instance !== this.instanceId;
    if (
      switching &&
      (!instance || resyncFrom !== this.instanceId || this.connection.state.get() === 'open')
    )
      return null;
    const previous = switching ? null : this.current.get();
    if (summary && previous && summary.id < previous.id) return null;
    // A same-instance null summary preserves the run and must not supersede an in-progress adoption.
    if (!summary && !switching && (instance === undefined || instance === this.instanceId))
      return {
        revision: this.adoptionRevision,
        instance: this.instanceId,
        session: previous?.id ?? null,
      };
    const revision = ++this.adoptionRevision;
    const identity = instance !== undefined && (summary || !previous) ? instance : this.instanceId;
    const next = !summary
      ? switching
        ? null
        : previous
      : previous?.id === summary.id
        ? applySummary(
            previous,
            summary,
            false,
            this.connection.state.get() === 'open' && summary.snapshot_rev == null,
          )
        : sessionFromSummary(summary);
    const changedInstance = !!identity && identity !== this.instanceId;
    this.instanceId = identity;
    const adoption: Adoption = { revision, instance: identity, session: next?.id ?? null };
    this.stopTarget = next && identity ? { session: next.id, instance: identity } : null;
    const notifySession = this.current.setDeferred(next);
    const notifyAllowed = switching ? this.allowedValue.setDeferred(null) : () => undefined;
    if (changedInstance && identity)
      this.instanceListener(identity, () => this.isCurrent(revision));
    // Each deferred notification drops itself if a nested update changed that value.
    notifySession();
    notifyAllowed();
    if (!this.sameRun(adoption)) return null;
    this.adoptAllowed(summary?.allowed ?? null);
    return this.sameRun(adoption) ? adoption : null;
  }

  sameRun(adoption: Adoption): boolean {
    return (
      this.instanceId === adoption.instance && (this.current.get()?.id ?? null) === adoption.session
    );
  }

  isCurrent(revision: number): boolean {
    return revision === this.adoptionRevision;
  }

  /**
   * Takes what the server accepts (an HTTP answer or a notification): the one with the larger `rev` wins (rev 0 is a
   * server that does not number it: the newest arrival wins). Used inside the library.
   */
  adoptAllowed(allowed: ServerAllowed | null): void {
    if (!allowed) return;
    const current = this.allowedValue.get();
    if (!current || allowed.rev === 0 || allowed.rev >= current.rev) this.allowedValue.set(allowed);
  }

  report(message: string, count = 1): void {
    this.notify(message, count);
  }

  dismissNotice(id: number): void {
    this.noticeList.update((list) => list.filter((notice) => notice.id !== id));
  }

  /** Reads and applies a batch from the connection. */
  private receive(raw: readonly unknown[]): void {
    const snapshotVersion = this.snapshotVersion();
    const epoch = this.connection.batchEpoch();
    let originInstance = this.instanceId;
    const currentOrigin = () =>
      this.connection.batchEpoch() === epoch && this.instanceId === originInstance;
    const batch: ServerMessage[] = [];
    const accepted: unknown[] = [];
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
        if (!currentOrigin()) return;
        const type = (value as { type?: unknown } | null)?.type;
        if (
          type === 'hello' &&
          error instanceof ProtocolError &&
          error.path === 'message.vehicle_sil_gui'
        ) {
          // Version mismatch: waiting will not fix it, so do not reconnect
          const version = JSON.stringify((value as { vehicle_sil_gui?: unknown }).vehicle_sil_gui);
          incompatible =
            `the server speaks version ${version} of the GUI protocol (vehicle_sil_gui) and this client ` +
            `reads version ${PROTOCOL_VERSION}: use a client made for the server's version`;
          stop = 'incompatible';
          break;
        }
        if (type === 'hello' || type === 'session') {
          // The state cannot be rebuilt: mark not in sync and receive again from hello (drop the rest)
          stop = 'lost';
          break;
        }
        continue;
      }
      if (message === null) continue;
      batch.push(message);
      accepted.push(value);
      if (message.type === 'error' && message.code === 'unauthorized') {
        // Refused: take nothing more and do not reconnect (the screen tells the user to open it again)
        stop = 'unauthorized';
        break;
      }
    }
    let filtered = this.filterOlderRuns(batch, accepted);
    if (filtered.staleHello && stop === null) stop = 'lost';
    // Make the requests to the connection before notifying subscribers. Set the stop flag before disconnecting (so a
    // connect() called from within the disconnect notification does not connect)
    if (stop === 'unauthorized' || stop === 'incompatible') {
      this.stoppedFlag = true;
      this.connection.close();
    }
    if (!currentOrigin()) {
      if (stop === 'unauthorized' || stop === 'incompatible') this.stoppedFlag = false;
      return;
    }
    if (stop === 'lost') this.connection.restart();
    if (!currentOrigin()) return;
    // A close subscriber can synchronously observe a later run on the same server.
    filtered = this.filterOlderRuns(filtered.batch, filtered.raw);
    this.apply(filtered.batch, filtered.raw, snapshotVersion, currentOrigin, (instance) => {
      originInstance = instance;
    });
    if (!currentOrigin()) return;
    if (stop === 'lost' || stop === 'incompatible') this.synced.set(false);
    if (stop === 'unauthorized') this.rejectedValue.set(true);
    if (incompatible !== null) this.incompatibleValue.set(incompatible);
  }

  /** Keep messages at or after the run already observed over HTTP, within each server instance. */
  private filterOlderRuns(
    batch: readonly ServerMessage[],
    raw: readonly unknown[],
  ): {
    batch: ServerMessage[];
    raw: unknown[];
    staleHello: boolean;
  } {
    let instance = this.instanceId;
    let floor = this.current.get()?.id ?? null;
    let staleHello = false;
    const selected: ServerMessage[] = [];
    const selectedRaw: unknown[] = [];
    batch.forEach((message, index) => {
      if (message.type === 'hello') {
        const id = message.session?.id ?? null;
        if (instance === message.instance && floor !== null && (id === null || id < floor)) {
          // Ask for a fresh snapshot, but keep later messages of the current run, including ACKs.
          staleHello = true;
          return;
        }
        if (instance !== message.instance) {
          instance = message.instance;
          floor = id;
        } else if (id !== null) floor = id;
      } else {
        const id =
          message.type === 'session'
            ? message.session.id
            : 'session' in message && typeof message.session === 'number'
              ? message.session
              : null;
        if (id !== null && floor !== null && id < floor) return;
        if (message.type === 'session' && (floor === null || message.session.id > floor))
          floor = message.session.id;
      }
      selected.push(message);
      selectedRaw.push(raw[index]);
    });
    return { batch: selected, raw: selectedRaw, staleHello };
  }

  private apply(
    batch: readonly ServerMessage[],
    raw: readonly unknown[],
    snapshotVersion: number,
    currentOrigin: () => boolean,
    commitOrigin: (instance: string | null) => void,
  ): void {
    if (batch.length === 0) return;
    const revision = ++this.adoptionRevision;
    const before = this.current.get();
    const originalInstance = this.instanceId;
    let instance = originalInstance;
    let session = before;
    let events: LauncherEvent[] = [];
    const notices: string[] = [];
    let applied = false;
    let synced = this.synced.get();
    let allowedNow = this.allowedValue.get();
    const takeAllowed = (incoming: ServerAllowed | null | undefined, replace = false) => {
      if (!incoming) return;
      if (replace || !allowedNow || incoming.rev === 0 || incoming.rev >= allowedNow.rev) {
        allowedNow = incoming;
      }
    };
    // The resent events between hello and synced may overlap what arrived before the reconnect (not counted)
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
          // Rebuild from the summary. Only a reconnect that asked to continue from what it holds (with from_seq) keeps
          // the local log
          synced = false;
          replaying = true;
          const { session: summary, replay } = message;
          // hello is authoritative (a server that restarted may number from the start again)
          const staleSnapshot =
            !!summary &&
            instance === message.instance &&
            session?.id === summary.id &&
            session.snapshotRev != null &&
            (summary.snapshot_rev == null || summary.snapshot_rev < session.snapshotRev);
          takeAllowed(message.allowed ?? summary?.allowed ?? null, !staleSnapshot);
          const resumed =
            summary &&
            instance === message.instance &&
            session?.id === summary.id &&
            replay?.from_seq != null;
          instance = message.instance;
          session = !summary
            ? null
            : staleSnapshot && session
              ? session
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
        case 'allowed':
          takeAllowed(message.allowed);
          break;
        case 'session':
          takeAllowed(message.session.allowed);
          // The summary is authoritative: it arrives in the same order as events, so replace the state with the
          // server's state at that point (keep the log)
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
            // Reported through rejected (receive disconnects)
          } else if (session && message.session === session.id) {
            const { code, message: text, skipped } = message;
            session = addProblem(
              session,
              skipped === undefined ? { code, message: text } : { code, message: text, skipped },
            );
          } else {
            notices.push(message.message);
          }
          break;
        case 'resync':
          // Sent again shortly, starting with hello. Not in sync until then
          synced = false;
          replaying = true;
          break;
      }
    }
    flush();
    const stale =
      (session?.staleEvents ?? 0) - (before?.id === session?.id ? (before?.staleEvents ?? 0) : 0);
    if (!currentOrigin() || !this.isCurrent(revision)) return;
    this.instanceId = instance;
    commitOrigin(instance);
    this.stopTarget = session && instance ? { session: session.id, instance } : null;
    // HTTP can synchronously refresh this same run during a notification. It supersedes its own
    // snapshot, but not the ordered WebSocket messages (which include command acknowledgements).
    const sessionId = session?.id ?? null;
    const sameRun = () =>
      currentOrigin() &&
      this.instanceId === instance &&
      (this.current.get()?.id ?? null) === sessionId;
    const notifySession = this.current.setDeferred(session);
    const notifyAllowed = this.allowedValue.setDeferred(allowedNow);
    const notifySynced = this.synced.setDeferred(synced);
    if (instance && instance !== originalInstance) this.instanceListener(instance, sameRun);
    notifySession();
    notifyAllowed();
    notifySynced();
    if (!sameRun()) return;
    for (const notice of notices) {
      this.notify(notice);
      if (!sameRun()) return;
    }
    if (applied) this.receivedAt.set(Date.now());
    if (!sameRun()) return;
    if (stale > 0) this.notify(STALE_EVENTS_NOTICE, stale);
    if (!sameRun()) return;
    callEach(this.rawListeners, [raw, sameRun, snapshotVersion], this.onError, sameRun);
    if (!sameRun()) return;
    callEach(this.messageListeners, [batch], this.onError, sameRun);
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
