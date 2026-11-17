import { httpExamples, protocolExamples, webSocketExamples } from '../testing/protocol-examples';
import { readCheckResult, readFinding, readLauncherEvent, readRunResult } from './launcher';
import { ProtocolError, Unreadable } from './read';

// The launcher decides the shapes in launcher.ts, and the server passes them on unchanged. The parsers type the keys
// that screens need, and keep the launcher's object in `raw`, so a key that the launcher adds reaches screens and
// panels without a change to the library. This test reads the launcher's objects in the recorded examples
// (gui/protocol/examples/) and checks that every key is reachable from what the parsers return: as a typed field, or
// in `raw`. PASSED_THROUGH names the keys that only `raw` has, so that a new key is noticed and can be typed.

/**
 * Keys of the launcher's objects that the parsers do not type: screens and panels read them from `raw`. As
 * `<object>: <key path>` (`[]` stands for every element of an array). A check result inside a run result or an event
 * is listed once, under `check result`. The format versions (`vehicle_sil_check`, `vehicle_sil_run`) are checked by
 * the parsers and passed through too.
 */
const PASSED_THROUGH: readonly string[] = [
  'check result: vehicle_sil_check',
  'check result: vehicle_sil',
  'check result: strict',
  'check result: registry_uri',
  'check result: start_timeout_ms',
  'check result: stop_timeout_ms',
  'check result: participants[].command',
  'check result: participants[].describe',
  'check result: participants[].reported',
  'check result: participants[].variables[].description',
  'check result: participants[].variables[].items',
  'check result: participants[].variables[].step',
  'check result: participants[].network_map',
  'check result: participants[].j1939_nodes',
  'check result: scenario.inputs',
  'check result: scenario.lines',
  'check result: networks[].line',
  // --connect: where the network is connected (null without it); the GUI cannot give --connect yet
  'check result: networks[].outside',
  // whether a run that is connected outside keeps real time, and because of what; the GUI does not show it yet
  'check result: time_lock',
  'run result: vehicle_sil_run',
  'run result: vehicle_sil',
  'run result: strict',
  // Where the run's registry listened: the same as the uri of the registry event, which the parser types
  'run result: registry_uri',
  // --connect: the bridges the run started (for_network null for a participant of the file) and the networks
  // connected (empty without --connect); the GUI cannot give --connect yet
  'run result: participants[].for_network',
  'run result: outside',
  'run result: scenario.inputs',
  'run result: scenario.lines',
  // The file of --timeline as the launcher was given it (the GUI server's temporary file): screens use the summary's
  // scenario.timeline_url instead
  'run result: scenario.timeline',
  'event participant: command',
  'event participant: log',
  'event log: colour',
  // Why a command was not accepted, as a code, and the sender's number of a stdin command (null: the GUI server
  // sends none yet)
  'event command: reason',
  'event command: id',
];

/**
 * Launcher events that the parser does not know, and why an example has them. readLauncherEvent returns null for them
 * (they are skipped, so subscribers of onMessages do not get them); the raw notification reaches
 * `client.connection.onBatch`.
 */
const UNKNOWN_EVENTS: Record<string, string> = {
  mystery: 'an event that a newer launcher may write',
  // Where the run stands and which operations it accepts now (docs/simulation-events.md): not read here yet
  state: 'the state of the run, which every run writes',
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

/** `raw` of what a parser returned (an own property that is not enumerable, so it is read by its descriptor). */
const rawOf = (read: unknown): unknown =>
  isObject(read) ? Object.getOwnPropertyDescriptor(read, 'raw')?.value : undefined;

interface LauncherObject {
  kind: string;
  raw: unknown;
  /** What the parser returned (null: an unknown event). */
  read: unknown;
}

/** Objects that the parsers copy as plain records (every key typed, so they have no `raw`). */
const RECORDS: readonly string[] = ['event participant: environment', 'event log_dropped: sources'];

interface Reach {
  /** Keys that only `raw` has. */
  untyped: Set<string>;
  /** Keys that neither a typed field nor `raw` has. */
  lost: Set<string>;
  /** Objects inside whose parsed object has no `raw`, or another object's. */
  notRaw: Set<string>;
}

/**
 * Walks raw and what the parser returned for it (typed: the typed fields; kept: its `raw`), recursively through the
 * typed fields (`[]` for the elements of an array). A nested launcher object that typed holds unchanged is visited on
 * its own (NESTED), so it is skipped here.
 */
function walk(raw: unknown, typed: unknown, kept: unknown, path: string, out: Reach): void {
  if (raw === typed) return;
  if (isObject(raw) && isObject(typed)) {
    if (path !== '' && kept !== raw) out.notRaw.add(path);
    for (const [key, value] of Object.entries(raw)) {
      const at = path === '' ? key : `${path}.${key}`;
      if (key in typed) walk(value, typed[key], rawOf(typed[key]), at, out);
      else if (isObject(kept) && kept[key] === value) out.untyped.add(at);
      else out.lost.add(at);
    }
  } else if (Array.isArray(raw) && Array.isArray(typed) && raw.length === typed.length) {
    raw.forEach((item, i) => walk(item, typed[i], rawOf(typed[i]), `${path}[]`, out));
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

describe('the keys of the launcher objects', () => {
  it('all reach what the parsers return, as typed fields or in raw (the untyped ones are listed)', async () => {
    const objects = await launcherObjects();
    const kinds = new Set(objects.map((o) => o.kind));
    for (const kind of ['check result', 'run result', 'finding', 'event check', 'event result']) {
      expect(kinds, `the examples have no whole ${kind}`).toContain(kind);
    }
    const reach: Reach = { untyped: new Set(), lost: new Set(), notRaw: new Set() };
    const unknownEvents = new Set<string>();
    // The loop also visits the nested objects it appends to objects.
    for (const { kind, raw, read } of objects) {
      if (read === null) {
        unknownEvents.add(kind.replace(/^event /, ''));
        continue;
      }
      if (!isObject(raw) || !isObject(read)) continue;
      if (rawOf(read) !== raw) reach.notRaw.add(kind);
      let typed: Fields = { ...read };
      const group = REGROUPED[kind];
      if (group !== undefined && isObject(read[group])) {
        if (rawOf(read[group]) !== raw) reach.notRaw.add(`${kind}: ${group}`);
        typed = { ...typed, ...read[group] };
      }
      for (const [key, nestedKind] of Object.entries(NESTED[kind] ?? {})) {
        if (isObject(raw[key]) && isObject(read[key])) {
          objects.push({ kind: nestedKind, raw: raw[key], read: read[key] });
          typed = { ...typed, [key]: raw[key] };
        }
      }
      const one: Reach = { untyped: new Set(), lost: new Set(), notRaw: new Set() };
      walk(raw, typed, raw, '', one);
      for (const key of one.untyped) reach.untyped.add(`${kind}: ${key}`);
      for (const key of one.lost) reach.lost.add(`${kind}: ${key}`);
      for (const key of one.notRaw) reach.notRaw.add(`${kind}: ${key}`);
    }
    expect(
      [...reach.notRaw].filter((key) => !RECORDS.includes(key)).sort(),
      'the parsers in protocol/launcher.ts returned objects whose raw is not the launcher object they read: wrap ' +
        'the object the parser returns in withRaw(f, {...})',
    ).toEqual([]);
    expect(
      [...reach.lost].sort(),
      'the examples have keys of launcher objects that the parsers in protocol/launcher.ts neither type nor keep ' +
        'in raw: read them in the parser, or wrap the object it returns in withRaw(f, {...})',
    ).toEqual([]);
    expect(
      [...reach.untyped].filter((key) => !PASSED_THROUGH.includes(key)).sort(),
      'the examples have keys of launcher objects that the parsers do not type (screens find them only in raw): ' +
        'type them in protocol/launcher.ts if screens need them, or add them to PASSED_THROUGH in this file',
    ).toEqual([]);
    expect(
      PASSED_THROUGH.filter((key) => !reach.untyped.has(key)).sort(),
      'PASSED_THROUGH lists keys that no example has, or that the parsers now type: remove them from it',
    ).toEqual([]);
    expect(
      [...unknownEvents].sort(),
      'the examples have launcher events that readLauncherEvent does not know (it skips them): read them in the ' +
        'parser, or add them to UNKNOWN_EVENTS in this file',
    ).toEqual(Object.keys(UNKNOWN_EVENTS).sort());
  });
});

/** The run result of the `result` event in an example. */
async function exampleRunResult(name: string): Promise<Fields> {
  const example = (await protocolExamples()).get(name) as { server: unknown[] };
  for (const message of example.server) {
    if (isObject(message) && isObject(message['event']) && message['event']['event'] === 'result') {
      return message['event']['result'] as Fields;
    }
  }
  throw new Error(`${name} has no result event`);
}

describe('raw of the launcher objects', () => {
  it('holds a key the library does not know, and stays out of JSON, keys and spreading', async () => {
    const launcher = { ...(await exampleRunResult('ws-pass')), future: { kind: 'new', n: [1, 2] } };
    const result = readRunResult(launcher, 'result');
    expect(result.raw).toBe(launcher);
    expect(result.raw?.['future']).toEqual({ kind: 'new', n: [1, 2] });
    expect(Object.keys(result)).not.toContain('raw');
    expect(JSON.stringify(result)).not.toContain('"raw"');
    expect({ ...result }.raw).toBeUndefined();
    expect(() => {
      (result as { raw?: unknown }).raw = {};
    }).toThrow(TypeError);
  });

  it('types the traffic, the log file and the role', async () => {
    const result = readRunResult(await exampleRunResult('ws-pass'), 'result');
    const network = result.traffic?.networks[0];
    expect(network?.name).toBe('J1939_CAN');
    expect(network?.delivered).toBe(true);
    expect(network?.participants.map((p) => [p.name, p.frames_sent, p.frames_received])).toEqual([
      ['Engine', 11, 0],
      ['Dashboard', 0, 11],
    ]);
    expect(result.traffic?.unplaced).toEqual([
      { participant: 'Logger', reason: 'no_control_link' },
    ]);
    expect(result.participants.map((p) => p.log)).toContain(null);
    expect(result.check?.participants.map((p) => p.role)).toContain('ecu');
  });

  it('is null for an older launcher that does not write the typed keys', async () => {
    const older: Fields = { ...(await exampleRunResult('ws-pass')) };
    delete older['traffic'];
    const result = readRunResult(older, 'result');
    expect(result.traffic).toBeNull();
  });

  it('does not loosen the typed keys: a typed key of the wrong type is still an error', async () => {
    const launcher = await exampleRunResult('ws-pass');
    const traffic = launcher['traffic'] as Fields;
    const network = (traffic['networks'] as Fields[])[0];
    expect(() => readRunResult({ ...launcher, traffic: 'none' }, 'result')).toThrow(ProtocolError);
    expect(() =>
      readRunResult(
        { ...launcher, traffic: { ...traffic, networks: [{ ...network, delivered: 'yes' }] } },
        'r',
      ),
    ).toThrow(/r\.traffic\.networks\[0\]\.delivered/);
    const u = new Unreadable('result');
    const partly = readRunResult({ ...launcher, traffic: 'none' }, 'result', u);
    expect(partly.traffic).toBeNull();
    expect(u.parts).toEqual(['traffic']);
  });
});
