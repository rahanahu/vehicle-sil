import { Connection } from './connection';
import {
  AuthMessage,
  LauncherEvent,
  PROTOCOL_VERSION,
  ProtocolError,
  RuntimeStatus,
  ServerAllowed,
  ServerMessage,
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

/** Identity of one run across all starts of the server. */
export interface RunTarget {
  run_id: string;
  instance: string;
}

export function sameRunTarget(left: RunTarget | null, right: RunTarget | null): boolean {
  return (
    left !== null &&
    right !== null &&
    left.run_id === right.run_id &&
    left.instance === right.instance
  );
}

/** Read side of the session copy (GuiClient.session). */
export interface ClientSession {
  /** The current (or last) session. null while the server has none yet. */
  readonly session: Readable<SessionView | null>;
  /** Identity of the session suitable for a targeted action. */
  currentTarget(): RunTarget | null;
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
  /** Persistent launcher status reported on hello and runtime notifications. */
  readonly runtime: Readable<RuntimeStatus | null>;
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

/**
 * Copy of a session (one run of the launcher). Reads the notifications of the connection (Connection) and builds the
 * state from the server's single WebSocket stream (summary and events).
 *
 * - Rebuilds the state from the summary in hello (the summary is authoritative), then applies the events that follow
 *   (the resent log and new events).
 * - On every connect, puts into the authentication message where in the stream it holds the server should resend
 *   from (`{instance, run_id, after_stream_seq}` via `resumeFields`).
 * - On a hello or session whose summary core is unreadable, marks itself not in sync and asks the connection to
 *   restart (receive again from hello).
 * - Passes unknown notifications and unknown events through without applying them (onMessages gets only the readable
 *   ones, the connection's onBatch gets all of them).
 * - Requests to the connection (disconnect after a refusal, restart) are made before subscribers are notified (so
 *   they happen even if a subscriber throws).
 */
export class SessionFeed implements ClientSession {
  private readonly rawListeners = new Set<
    (batch: readonly unknown[], current: () => boolean) => void
  >();
  private instanceId: string | null = null;
  private lastStreamSeq = -1;
  private resyncing = false;
  private stopTarget: RunTarget | null = null;
  private readonly current: Writable<SessionView | null>;
  private readonly synced: Writable<boolean>;
  private readonly allowedValue: Writable<ServerAllowed | null>;
  private readonly runtimeValue: Writable<RuntimeStatus | null>;
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
  readonly runtime: Readable<RuntimeStatus | null>;
  readonly lastEventAt: Readable<number>;
  readonly notices: Readable<readonly Notice[]>;
  readonly rejected: Readable<boolean>;
  readonly incompatible: Readable<string | null>;

  constructor(
    private readonly connection: Connection,
    private readonly onError: ErrorSink = rethrowLater,
    private readonly onInstance?: (instance: string) => void,
    private readonly hooks?: {
      beforeRunSwitch?: (
        beforeBatch: readonly unknown[],
        skippedBatch: readonly unknown[],
        current: () => boolean,
      ) => void;
      stageRunSwitch?: (
        instance: string | null,
        sessionId: string | null,
        instanceChanged: boolean,
      ) => () => void;
    },
  ) {
    this.current = new Writable<SessionView | null>(null, Object.is, onError);
    this.synced = new Writable<boolean>(false, Object.is, onError);
    this.allowedValue = new Writable<ServerAllowed | null>(null, Object.is, onError);
    this.allowed = this.allowedValue.readonly();
    this.runtimeValue = new Writable<RuntimeStatus | null>(null, Object.is, onError);
    this.runtime = this.runtimeValue.readonly();
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
   * Fields to add to the authentication message: where in the stream it holds the server should resend from. Called
   * on every connect (after the call, the copy is not in sync until hello and the resent events have arrived). Used
   * only inside the library.
   */
  resumeFields(): Pick<AuthMessage, 'instance' | 'run_id' | 'after_stream_seq'> {
    this.synced.set(false);
    const session = this.current.get();
    if (this.instanceId === null || session === null || this.lastStreamSeq < 0) return {};
    return {
      instance: this.instanceId,
      run_id: session.id,
      after_stream_seq: this.lastStreamSeq,
    };
  }

  onMessages(listener: (batch: readonly ServerMessage[]) => void): () => void {
    this.messageListeners.add(listener);
    return () => {
      this.messageListeners.delete(listener);
    };
  }

  onRawMessages(listener: (batch: readonly unknown[], current: () => boolean) => void): () => void {
    this.rawListeners.add(listener);
    return () => this.rawListeners.delete(listener);
  }

  /** Instance associated with the current session, or null if its origin is not known. */
  currentInstance(): string | null {
    return this.instanceId;
  }

  currentTarget(): RunTarget | null {
    return this.stopTarget ? { ...this.stopTarget } : null;
  }

  report(message: string, count = 1): void {
    this.notify(message, count);
  }

  dismissNotice(id: number): void {
    this.noticeList.update((list) => list.filter((notice) => notice.id !== id));
  }

  /** Reads and applies a batch from the connection. */
  private receive(raw: readonly unknown[]): void {
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
    this.apply(batch, accepted, currentOrigin, (instance) => {
      originInstance = instance;
    });
    if (!currentOrigin()) return;
    if (stop === 'lost' || stop === 'incompatible') this.synced.set(false);
    if (stop === 'unauthorized') this.rejectedValue.set(true);
    if (incompatible !== null) this.incompatibleValue.set(incompatible);
  }

  private apply(
    batch: readonly ServerMessage[],
    raw: readonly unknown[],
    currentOrigin: () => boolean,
    commitOrigin: (instance: string | null) => void,
  ): void {
    if (batch.length === 0) return;
    const before = this.current.get();
    let instance = this.instanceId;
    let session = before;
    let streamSeq = this.lastStreamSeq;
    let resyncing = this.resyncing;
    let events: LauncherEvent[] = [];
    const notices: string[] = [];
    const selectedBatch: ServerMessage[] = [];
    const selectedRaw: unknown[] = [];
    let firstSwitchRawIndex = -1;
    let lastSwitchRawIndex = -1;
    let applied = false;
    let synced = this.synced.get();
    let syncReset = false;
    let allowedNow = this.allowedValue.get();
    let runtimeNow = this.runtimeValue.get();
    const takeAllowed = (incoming: ServerAllowed | null | undefined) => {
      if (incoming) {
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
    for (let i = 0; i < batch.length; i++) {
      const message = batch[i];
      const rawMessage = raw[i];
      if (
        message.type !== 'hello' &&
        message.type !== 'resync' &&
        message.type !== 'values' &&
        typeof message.stream_seq === 'number'
      ) {
        if (!replaying && message.stream_seq <= streamSeq) {
          continue;
        }
        streamSeq = Math.max(streamSeq, message.stream_seq);
      }
      if (message.type !== 'event') flush();
      switch (message.type) {
        case 'hello': {
          synced = false;
          syncReset = true;
          replaying = true;
          const { session: summary, replay } = message;
          const sameInstance = instance !== null && instance === message.instance;
          const sameRun = !!summary && sameInstance && session?.id === summary.id;
          const deltaResume = sameRun && !resyncing && replay?.after_stream_seq != null;
          resyncing = false;
          if (typeof message.stream_seq === 'number') {
            streamSeq = message.stream_seq;
          } else if (!sameInstance) {
            streamSeq = summary ? (summary.last_seq ?? 0) : -1;
          } else if (summary) {
            streamSeq = deltaResume
              ? Math.max(streamSeq, summary.last_seq ?? 0)
              : (summary.last_seq ?? 0);
          } else {
            streamSeq = -1;
          }
          takeAllowed(message.allowed ?? summary?.allowed ?? null);
          if (message.runtime !== undefined) {
            runtimeNow = message.runtime;
          } else if (!sameInstance) {
            runtimeNow = null;
          }
          const prevId = session?.id ?? null;
          const switchedInstance = instance !== null && instance !== message.instance;
          instance = message.instance;
          if (!summary) {
            session = null;
          } else if (deltaResume && session) {
            session = applySummary(session, summary);
          } else {
            session = sessionFromSummary(summary);
          }
          if (switchedInstance || (session?.id ?? null) !== prevId) {
            if (firstSwitchRawIndex < 0) firstSwitchRawIndex = selectedRaw.length;
            lastSwitchRawIndex = selectedRaw.length;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        }
        case 'dropped': {
          if (session && message.run_id === session.id) {
            session = addDropped(session, message);
          } else if (session && message.run_id !== null) {
            continue;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        }
        case 'synced': {
          if (message.run_id === null ? session === null : session?.id === message.run_id) {
            synced = true;
            replaying = false;
          } else {
            continue;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        }
        case 'allowed':
          if (message.run_id !== null && (!session || message.run_id !== session.id)) {
            continue;
          }
          takeAllowed(message.allowed);
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        case 'runtime':
          runtimeNow = message.runtime;
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        case 'session': {
          const prevId = session?.id ?? null;
          takeAllowed(message.session.allowed);
          session =
            session?.id === message.session.id
              ? applySummary(session, message.session)
              : sessionFromSummary(message.session);
          if (typeof message.stream_seq !== 'number' && message.session.last_seq !== null) {
            streamSeq = Math.max(streamSeq, message.session.last_seq);
          }
          if (session.id !== prevId) {
            if (firstSwitchRawIndex < 0) firstSwitchRawIndex = selectedRaw.length;
            lastSwitchRawIndex = selectedRaw.length;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        }
        case 'event':
          if (session?.id === message.run_id) {
            events.push(message.event);
            if (typeof message.stream_seq !== 'number') {
              streamSeq = Math.max(streamSeq, message.event.seq);
            }
            selectedBatch.push(message);
            selectedRaw.push(rawMessage);
          }
          break;
        case 'error':
          if (message.code === 'unauthorized') {
            // Reported through rejected (receive disconnects)
          } else if (session && message.run_id === session.id) {
            const { code, message: text, skipped } = message;
            session = addProblem(session, {
              code,
              message: text,
              ...(skipped === undefined ? {} : { skipped }),
            });
          } else if (message.run_id === null) {
            notices.push(message.message);
          } else {
            continue;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        case 'resync':
          // Sent again shortly, starting with hello. Not in sync until then
          synced = false;
          syncReset = true;
          replaying = true;
          resyncing = true;
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
        default:
          if (session !== null && 'run_id' in message && message.run_id !== session.id) {
            continue;
          }
          selectedBatch.push(message);
          selectedRaw.push(rawMessage);
          break;
      }
    }
    flush();
    const stale =
      (session?.staleEvents ?? 0) - (before?.id === session?.id ? (before?.staleEvents ?? 0) : 0);
    if (!currentOrigin()) return;
    const beforeInstance = this.instanceId;
    const beforeId = before?.id ?? null;
    if (lastSwitchRawIndex > 0 && this.hooks?.beforeRunSwitch) {
      const sameBefore = () =>
        currentOrigin() &&
        this.instanceId === beforeInstance &&
        (this.current.get()?.id ?? null) === beforeId;
      this.hooks.beforeRunSwitch(
        selectedRaw.slice(0, firstSwitchRawIndex),
        selectedRaw.slice(firstSwitchRawIndex, lastSwitchRawIndex),
        sameBefore,
      );
      if (!sameBefore()) return;
    }
    this.instanceId = instance;
    this.lastStreamSeq = streamSeq;
    this.resyncing = resyncing;
    commitOrigin(instance);
    this.stopTarget = session && instance ? { run_id: session.id, instance } : null;
    const sessionId = session?.id ?? null;
    const sameStream = () =>
      currentOrigin() &&
      this.instanceId === instance &&
      (this.current.get()?.id ?? null) === sessionId;
    const instanceChanged =
      instance !== null && beforeInstance !== null && instance !== beforeInstance;
    const notifySession = this.current.setDeferred(session);
    const notifyAllowed = this.allowedValue.setDeferred(allowedNow);
    const notifyRuntime = this.runtimeValue.setDeferred(runtimeNow);
    if (!synced || syncReset) {
      this.synced.set(false);
      if (!sameStream()) return;
    }
    let notifyVarSwitch: (() => void) | undefined;
    if (this.hooks?.stageRunSwitch) {
      if (lastSwitchRawIndex >= 0 || (instance !== null && instance !== beforeInstance)) {
        notifyVarSwitch = this.hooks.stageRunSwitch(instance, sessionId, instanceChanged);
      }
    } else if (instance !== null && instance !== beforeInstance) {
      this.onInstance?.(instance);
      if (!sameStream()) return;
    }
    notifySession();
    notifyAllowed();
    notifyRuntime();
    if (!sameStream()) return;
    notifyVarSwitch?.();
    if (!sameStream()) return;
    for (const notice of notices) {
      this.notify(notice);
      if (!sameStream()) return;
    }
    if (applied) this.receivedAt.set(Date.now());
    if (!sameStream()) return;
    if (stale > 0) this.notify(STALE_EVENTS_NOTICE, stale);
    if (!sameStream()) return;
    const rawToDeliver =
      this.hooks?.beforeRunSwitch && lastSwitchRawIndex >= 0
        ? selectedRaw.slice(lastSwitchRawIndex)
        : selectedRaw;
    if (rawToDeliver.length > 0) {
      callEach(this.rawListeners, [rawToDeliver, sameStream], this.onError, sameStream);
    }
    if (!sameStream()) return;
    // Mark synced and notify ready subscribers only after both session state and raw listeners
    // (VariablesFeed) have applied the batch, so ready=true never observes stale variable state.
    const notifySynced = this.synced.setDeferred(synced);
    notifySynced();
    if (!sameStream()) return;
    if (selectedBatch.length > 0) {
      callEach(this.messageListeners, [selectedBatch], this.onError, sameStream);
    }
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
