// What the front door shows about connecting a network outside the simulation: the list of where a network can be
// connected (`connections()`, from POST api/connections) laid out as one list, and the rows of the networks that a run
// connected (`outside`, from the launcher's `state` and `outside` events). Nothing here is a rule: the sentences are the
// launcher's and the bridges', and whether a target can be used is the launcher's `usable`.
import type { GuiClient } from './client';
import type { Connections, ServerMessage } from './protocol';
import type { ConnectEntry, Outside } from './front-types';
import { sameData } from './front-compose';
import { ErrorSink, Readable, Writable, derived } from './values';

const isObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const text = (value: unknown): string | null => (typeof value === 'string' ? value : null);

/**
 * The entries of every backend of the answer, in its order: the targets of a backend that listed, and one entry with
 * the launcher's sentence for a backend that could not (`list.error`). An error finding of the answer (a file that
 * cannot be read) is an entry of its own, after them. A target without a `value` is left out.
 */
export function entriesOf(answer: Connections): ConnectEntry[] {
  const entries: ConnectEntry[] = [];
  const networks = answer.connections['networks'];
  for (const network of Array.isArray(networks) ? networks : []) {
    if (!isObject(network) || !isObject(network['list'])) continue; // a LIN network has no list
    const list = network['list'];
    const backend = text(list['backend']) ?? text(network['backend']);
    for (const target of Array.isArray(list['targets']) ? list['targets'] : []) {
      if (!isObject(target)) continue;
      const value = text(target['value']);
      if (value === null) continue;
      entries.push({
        backend,
        value,
        label: text(target['label']) ?? value,
        detail: text(target['detail']) ?? '',
        usable: target['usable'] === true,
        reason: text(target['reason']),
      });
    }
    if (isObject(list['error'])) {
      entries.push({ backend, error: text(list['error']['message']) ?? 'the list failed' });
    }
  }
  const findings = answer.connections['findings'];
  for (const finding of Array.isArray(findings) ? findings : []) {
    if (isObject(finding) && finding['severity'] === 'error' && text(finding['message']) !== null) {
      entries.push({ backend: null, error: text(finding['message'])! });
    }
  }
  return entries;
}

/** The counts of the `outside` events of one run. */
interface Counts {
  session: number;
  networks: ReadonlyMap<string, { to: number; from: number }>;
}

/**
 * The rows of `front.outside`: `outside[]` of the launcher's last `state` while the launcher of the run runs, each with
 * the counts of the last `outside` event of its network in that run (null until one has arrived on this page: the
 * server does not send old ones again after a reconnect). Returns the value and the function that stops it.
 */
export function outsideRows(
  low: GuiClient,
  onError: ErrorSink,
): { value: Readable<readonly Outside[]>; stop: () => void } {
  const counts = new Writable<Counts | null>(null, Object.is, onError);
  const stop = low.session.onMessages((batch: readonly ServerMessage[]) => {
    let next = counts.get();
    for (const message of batch) {
      if (message.type !== 'event' || message.event.event !== 'outside') continue;
      const { network, to_outside, from_outside } = message.event;
      const networks = new Map(next?.session === message.session ? next.networks : []);
      networks.set(network, { to: to_outside, from: from_outside });
      next = { session: message.session, networks };
    }
    if (next !== counts.get()) counts.set(next);
  });
  const value = derived(
    [low.session.session, counts],
    (): readonly Outside[] => {
      const session = low.session.session.get();
      if (!session?.launcher.running) return [];
      const known = counts.get();
      const of = known?.session === session.id ? known.networks : new Map();
      return (session.launcherState?.outside ?? []).map((row) => {
        const count = of.get(row.network);
        return {
          network: row.network,
          target: row.target,
          participant: row.participant,
          link: row.link,
          bus: row.bus,
          condition: row.condition,
          reasons: row.reasons,
          nothing_received: row.nothing_received,
          to_outside: count?.to ?? null,
          from_outside: count?.from ?? null,
        };
      });
    },
    sameData,
    onError,
  );
  return { value, stop };
}
