/**
 * Readable value: the current value (get) and change notifications (subscribe). All of the library's state comes in
 * this form.
 *
 * - Plain JavaScript can use it directly (no framework needed). A framework wrapper (an Angular signal and so on) takes
 *   the first value with get() and mirrors later values received through subscribe().
 * - subscribe(listener) calls listener whenever the value changes (not with the current value). Calling the returned
 *   function unsubscribes.
 * - Notifications are synchronous: they arrive inside the code that changed the value. A listener that unsubscribes
 *   during a round of notifications still gets that round's notification.
 * - If a listener changes the same value, the new value reaches every listener and the rest of the notifications for
 *   the old value are dropped (an old value never arrives later).
 * - A listener that throws stops neither the notification of the other listeners nor the library's state updates.
 *   What it threw goes to the client's `onError` (an option of createClient).
 */
export interface Readable<T> {
  get(): T;
  subscribe(listener: (value: T) => void): () => void;
}

/** Receives what a subscriber threw. */
export type ErrorSink = (error: unknown) => void;

/** Default ErrorSink: rethrows later (never in the middle of notifying). */
export const rethrowLater: ErrorSink = (error) =>
  queueMicrotask(() => {
    throw error;
  });

/** Calls each listener. If one throws, passes it to onError and moves on. */
export function callEach<A extends unknown[]>(
  listeners: Iterable<(...args: A) => void>,
  args: A,
  onError: ErrorSink,
  stillCurrent: () => boolean = () => true,
): void {
  for (const listener of [...listeners]) {
    if (!stillCurrent()) return;
    try {
      listener(...args);
    } catch (error) {
      try {
        onError(error);
      } catch (sinkError) {
        // Even if onError itself throws, the remaining listeners still get notified
        rethrowLater(sinkError);
      }
    }
  }
}

/** Writable value. The library keeps its state in these and exposes them read-only through readonly(). */
export class Writable<T> implements Readable<T> {
  private value: T;
  private version = 0;
  private readonly listeners = new Set<(value: T) => void>();

  constructor(
    initial: T,
    /** Equality test. Equal values are not notified. */
    private readonly equal: (a: T, b: T) => boolean = Object.is,
    /** Receives what a subscriber threw. */
    private readonly onError: ErrorSink = rethrowLater,
  ) {
    this.value = initial;
  }

  get(): T {
    return this.value;
  }

  set(value: T): void {
    if (this.equal(this.value, value)) return;
    this.value = value;
    const version = ++this.version;
    // If the value changed again during notification, the remaining subscribers already got the new value: skip the old
    // one
    callEach(this.listeners, [value], this.onError, () => this.version === version);
  }

  update(change: (value: T) => T): void {
    this.set(change(this.value));
  }

  subscribe(listener: (value: T) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** Read-only view (get and subscribe only). */
  readonly(): Readable<T> {
    return { get: () => this.get(), subscribe: (listener) => this.subscribe(listener) };
  }
}

/**
 * Value derived from other readable values. Recomputed with compute whenever a source changes, and notified only when
 * equal says it differs. Stays subscribed to the sources for good (use it for values that live as long as the
 * derived value, for example inside the client).
 */
export function derived<T>(
  sources: readonly Readable<unknown>[],
  compute: () => T,
  equal: (a: T, b: T) => boolean = Object.is,
  onError: ErrorSink = rethrowLater,
): Readable<T> {
  const value = new Writable(compute(), equal, onError);
  for (const source of sources) source.subscribe(() => value.set(compute()));
  return value.readonly();
}

/** Shallow equality (every field of the objects is the same by Object.is). Used for state snapshots. */
export function shallowEqual<T extends object>(a: T, b: T): boolean {
  if (Object.is(a, b)) return true;
  const keys = Object.keys(a) as (keyof T)[];
  return keys.length === Object.keys(b).length && keys.every((key) => Object.is(a[key], b[key]));
}
