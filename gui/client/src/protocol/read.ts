/**
 * Building blocks of the parsers in launcher.ts and server.ts: the error, the readers of single values, and reading
 * part by part (Unreadable). All parsers follow these rules:
 * - Unknown fields are ignored. Unknown events and unknown notifications become null and are skipped.
 * - Fields whose set of values may grow (phase, state, result and so on) pass unknown values through as strings.
 * - Data of the wrong shape (missing field, wrong type, unknown format version) throws ProtocolError.
 * - Exception: the session summary and launcher events are read part by part (readSessionSummary, readServerMessage).
 *   If a part that comes from the launcher (check, result, findings, participants) or a part the server shrank to fit
 *   its limits cannot be read, the readable parts are still used; an unreadable part gets its default value and its
 *   name goes into `unreadable`. Only when the summary core (id, state, launcher, stop) or the event core (seq, event)
 *   cannot be read is the whole thing unreadable.
 */

/** Data of an unreadable shape. path is where it broke (for example `event.participants[0].name`). */
export class ProtocolError extends Error {
  constructor(
    readonly path: string,
    readonly expected: string,
  ) {
    super(`${path}: expected ${expected}`);
    this.name = 'ProtocolError';
  }
}

/** A string whose set of values may grow. Known values get completion; unknown values are allowed too. */
export type Open<T extends string> = T | (string & {});

// --- Parsing helpers -----------------------------------------------------------------------------

/**
 * Reads a received value. path is how the error location is shown (for example `message.session`). Throws ProtocolError
 * on the wrong shape.
 */
export type Reader<T> = (value: unknown, path: string) => T;
export type Fields = Record<string, unknown>;

export function object(value: unknown, path: string): Fields {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new ProtocolError(path, 'an object');
  }
  return value as Fields;
}

export const string: Reader<string> = (value, path) => {
  if (typeof value !== 'string') throw new ProtocolError(path, 'a string');
  return value;
};

export const number: Reader<number> = (value, path) => {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new ProtocolError(path, 'a number');
  }
  return value;
};

export const integer: Reader<number> = (value, path) => {
  if (!Number.isInteger(value)) throw new ProtocolError(path, 'an integer');
  return value as number;
};

export const boolean: Reader<boolean> = (value, path) => {
  if (typeof value !== 'boolean') throw new ProtocolError(path, 'true or false');
  return value;
};

export function nullable<T>(read: Reader<T>): Reader<T | null> {
  return (value, path) => (value === null ? null : read(value, path));
}

/** fallback when the field is absent (undefined). For fields that older launchers do not write yet. */
export function optional<T, F>(read: Reader<T>, fallback: F): Reader<T | F> {
  return (value, path) => (value === undefined ? fallback : read(value, path));
}

export function array<T>(read: Reader<T>): Reader<T[]> {
  return (value, path) => {
    if (!Array.isArray(value)) throw new ProtocolError(path, 'an array');
    return value.map((item, i) => read(item, `${path}[${i}]`));
  };
}

export function field<T>(fields: Fields, path: string, key: string, read: Reader<T>): T {
  return read(fields[key], `${path}.${key}`);
}

/**
 * Collects the names of the parts that could not be read when reading part by part. Names are relative to root,
 * without array indexes (`message.session.participants[2].signal` → `participants.signal`).
 */
export class Unreadable {
  private readonly names = new Set<string>();

  constructor(
    private readonly root: string,
    private readonly prefix = '',
  ) {}

  add(path: string): void {
    const relative = path.startsWith(`${this.root}.`) ? path.slice(this.root.length + 1) : path;
    this.names.add(this.prefix + relative.replace(/\[\d+\]/g, ''));
  }

  get parts(): string[] {
    return [...this.names];
  }
}

/**
 * If unreadable, uses fallback and records the location in u (throws without u). An absent field counts as unreadable.
 */
export function lenient<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  if (!u) return read;
  return (value, path) => {
    try {
      return read(value, path);
    } catch (error) {
      if (!(error instanceof ProtocolError)) throw error;
      u.add(error.path);
      return fallback;
    }
  };
}

/**
 * An absent field silently becomes fallback; a field of the wrong type becomes fallback and is recorded in u (throws
 * without u).
 */
export function soft<T, F>(read: Reader<T>, fallback: F, u?: Unreadable): Reader<T | F> {
  return lenient(optional(read, fallback), fallback, u);
}

/** An array. With u, unreadable elements are skipped and recorded in u (empty if absent or not an array). */
export function items<T>(read: Reader<T>, u?: Unreadable): Reader<T[]> {
  if (!u) return array(read);
  return (value, path) => {
    if (!Array.isArray(value)) {
      u.add(path);
      return [];
    }
    const out: T[] = [];
    value.forEach((item, i) => {
      try {
        out.push(read(item, `${path}[${i}]`));
      } catch (error) {
        if (!(error instanceof ProtocolError)) throw error;
        u.add(error.path);
      }
    });
    return out;
  };
}

/**
 * A format version field. An unknown version is not read (its meaning may have changed). When reading part by part
 * (u), it is recorded as an unreadable part and the readable fields are still read.
 */
export function version(
  fields: Fields,
  path: string,
  key: string,
  expected: number,
  u?: Unreadable,
): void {
  if (fields[key] !== expected) {
    const error = new ProtocolError(`${path}.${key}`, `format version ${expected}`);
    if (!u) throw error;
    u.add(error.path);
  }
}

export function record<T>(read: Reader<T>): Reader<Record<string, T>> {
  return (value, path) =>
    Object.fromEntries(
      Object.entries(object(value, path)).map(([key, item]) => [key, read(item, `${path}.${key}`)]),
    );
}
