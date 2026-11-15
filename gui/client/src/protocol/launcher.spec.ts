import { httpExamples, webSocketExamples } from '../testing/protocol-examples';
import { readCheckResult, readFinding, readLauncherEvent, readRunResult } from './launcher';
import { Unreadable } from './read';

// The launcher decides the shapes in launcher.ts, and the server passes them on unchanged: a key the launcher adds
// reaches the library without anyone changing the library. This test reads the launcher's objects in the recorded
// examples (gui/protocol/examples/) and names every key the parsers do not return, so that a new one is not lost
// unnoticed.

/**
 * Keys of the launcher's objects that the parsers do not return, as `<object>: <key path>` (`[]` stands for every
 * element of an array). A check result inside a run result or an event is listed once, under `check result`.
 * - `dropped`: the parser leaves the key out, so screens and panels cannot see it (only the raw message of
 *   `client.connection.onBatch` has it).
 * - `checked`: a format version that the parser checks and does not return.
 */
const NOT_RETURNED: Record<string, 'dropped' | 'checked'> = {
  'check result: vehicle_sil_check': 'checked',
  'check result: vehicle_sil': 'dropped',
  'check result: strict': 'dropped',
  'check result: registry_uri': 'dropped',
  'check result: start_timeout_ms': 'dropped',
  'check result: stop_timeout_ms': 'dropped',
  'check result: participants[].command': 'dropped',
  'check result: participants[].describe': 'dropped',
  'check result: participants[].reported': 'dropped',
  'check result: participants[].role': 'dropped',
  'check result: participants[].variables[].description': 'dropped',
  'check result: participants[].variables[].items': 'dropped',
  'check result: participants[].variables[].step': 'dropped',
  'check result: networks[].line': 'dropped',
  'run result: vehicle_sil_run': 'checked',
  'run result: vehicle_sil': 'dropped',
  'run result: strict': 'dropped',
  'run result: traffic': 'dropped',
  'run result: participants[].log': 'dropped',
  'event participant: command': 'dropped',
  'event participant: log': 'dropped',
  'event log: colour': 'dropped',
};

/** Launcher events that the parser does not know (it returns null and they are skipped), and why an example has them. */
const UNKNOWN_EVENTS: Record<string, string> = {
  mystery: 'an event that a newer launcher may write',
};

/** Events whose parser puts the event's fields into one property (`finding`, `step`, `link`). */
const REGROUPED: Record<string, string> = {
  'event finding': 'finding',
  'event scenario_step': 'step',
  'event control_link': 'link',
};

/** Objects inside an object that are launcher objects of their own: their keys are listed under their own kind. */
const NESTED: Record<string, Record<string, string>> = {
  'run result': { check: 'check result' },
  'event check': { check: 'check result' },
  'event result': { result: 'run result' },
};

type Fields = Record<string, unknown>;

const isObject = (value: unknown): value is Fields =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

interface LauncherObject {
  kind: string;
  raw: unknown;
  /** What the parser returned (null: an unknown event). */
  read: unknown;
}

/** Adds to out the keys of raw that read lacks, recursively (`[]` for the elements of an array). */
function notReturned(raw: unknown, read: unknown, path: string, out: Set<string>): void {
  if (isObject(raw) && isObject(read)) {
    for (const [key, value] of Object.entries(raw)) {
      const at = path === '' ? key : `${path}.${key}`;
      if (key in read) notReturned(value, read[key], at, out);
      else out.add(at);
    }
  } else if (Array.isArray(raw) && Array.isArray(read) && raw.length === read.length) {
    raw.forEach((item, i) => notReturned(item, read[i], `${path}[]`, out));
  }
}

/**
 * The launcher's objects in the examples: check results, run results, findings and events. Objects that are not whole
 * on purpose (an example of a part the parser cannot read, or of a part the server shrank) are left out.
 */
async function launcherObjects(): Promise<LauncherObject[]> {
  const found: LauncherObject[] = [];
  const add = (
    kind: string,
    raw: unknown,
    read: (value: unknown, path: string, u: Unreadable) => unknown,
  ) => {
    if (raw === null || raw === undefined) return;
    const u = new Unreadable(kind);
    const value = read(raw, kind, u);
    if (u.parts.length === 0) found.push({ kind, raw, read: value });
  };
  const summary = (raw: unknown) => {
    if (!isObject(raw)) return;
    add('check result', raw['check'], readCheckResult);
    add('run result', raw['result'], readRunResult);
    for (const finding of (raw['findings'] as unknown[] | undefined) ?? []) {
      add('finding', finding, readFinding);
    }
  };
  for (const example of await httpExamples()) {
    if (example.status >= 400 || !isObject(example.response)) continue;
    if (example.request.path === 'api/check') {
      add('check result', example.response['check'], readCheckResult);
    }
    summary(example.response['session']);
  }
  for (const example of await webSocketExamples()) {
    for (const message of example.server) {
      if (!isObject(message)) continue;
      if (message['type'] === 'hello' || message['type'] === 'session') summary(message['session']);
      if (message['type'] !== 'event' || !isObject(message['event'])) continue;
      const event = message['event'];
      const read = readLauncherEvent(event, 'event', true);
      if (!read?.unreadable)
        found.push({ kind: `event ${String(event['event'])}`, raw: event, read });
    }
  }
  return found;
}

describe('the keys of the launcher objects that the parsers do not return', () => {
  it('are exactly the listed ones, so that a key the launcher adds is not lost unnoticed', async () => {
    const objects = await launcherObjects();
    const kinds = new Set(objects.map((o) => o.kind));
    for (const kind of ['check result', 'run result', 'finding', 'event check', 'event result']) {
      expect(kinds, `the examples have no whole ${kind}`).toContain(kind);
    }
    const found = new Set<string>();
    const unknownEvents = new Set<string>();
    // The loop also visits the nested objects it appends to objects.
    for (const { kind, raw, read } of objects) {
      if (read === null) {
        unknownEvents.add(kind.replace(/^event /, ''));
        continue;
      }
      if (!isObject(raw) || !isObject(read)) continue;
      let compared: Fields = read;
      const group = REGROUPED[kind];
      if (group !== undefined && isObject(read[group])) {
        compared = { ...read, ...read[group] };
      }
      for (const [key, nestedKind] of Object.entries(NESTED[kind] ?? {})) {
        if (isObject(raw[key]) && isObject(read[key])) {
          objects.push({ kind: nestedKind, raw: raw[key], read: read[key] });
          compared = { ...compared, [key]: raw[key] };
        }
      }
      const keys = new Set<string>();
      notReturned(raw, compared, '', keys);
      for (const key of keys) found.add(`${kind}: ${key}`);
    }
    const listed = Object.keys(NOT_RETURNED);
    expect(
      [...found].filter((key) => !listed.includes(key)).sort(),
      'the examples have keys of launcher objects that the parsers in protocol/launcher.ts do not return: read ' +
        'them in the parser, or add them to NOT_RETURNED in this file',
    ).toEqual([]);
    expect(
      listed.filter((key) => !found.has(key)).sort(),
      'NOT_RETURNED lists keys that no example has, or that the parsers now return: remove them from it',
    ).toEqual([]);
    expect(
      [...unknownEvents].sort(),
      'the examples have launcher events that readLauncherEvent does not know (it skips them): read them in the ' +
        'parser, or add them to UNKNOWN_EVENTS in this file',
    ).toEqual(Object.keys(UNKNOWN_EVENTS).sort());
  });
});
