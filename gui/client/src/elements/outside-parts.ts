// The parts of connecting a network outside the simulation: choose where a network of the next run is connected, and
// show the networks that the run connected. They have no rules: the sentences are the launcher's, the bridges' and the
// GUI server's, and whether a target can be chosen is the launcher's `usable`.
import { watch } from '../index';
import type { CheckResult, ConnectEntry, Finding, Front, Outside } from '../index';
import { pageIntent } from './page-intent';
import { Part, h, setData } from './part';

let ids = 0;

/** The code of an ActionError (else `error`). */
const codeOf = (error: unknown): string => {
  const code: unknown = Reflect.get(Object(error), 'code');
  return typeof code === 'string' && code !== '' ? code : 'error';
};
const messageOf = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

/** The words of the first choice: the network stays on SIL Kit only. */
const NONE_TEXT = 'SIL Kit only (not connected)';

const prefixed = (finding: Finding, prefixes: readonly string[]) =>
  prefixes.some((prefix) => finding.code.startsWith(prefix));

/**
 * The findings of a check that are about connecting `network`: its own `connect-*` and `outside-*` findings, and, when
 * the check says that this network makes the run keep real time (`time_lock.because`), the `outside-*` findings of the
 * whole run.
 */
function findingsFor(result: CheckResult, network: string): Finding[] {
  const lock: unknown = result.raw?.['time_lock'];
  const because: unknown =
    typeof lock === 'object' && lock !== null ? Reflect.get(lock, 'because') : null;
  const locks = Array.isArray(because) && because.includes(network);
  return result.findings.filter(
    (f) =>
      (f.network === network && prefixed(f, ['connect-', 'outside-'])) ||
      (locks && f.network === null && prefixed(f, ['outside-'])),
  );
}

/**
 * `<vsil-connect network="N">`: where network N of the next run is connected outside the simulation. The first choice
 * is SIL Kit only (`none`). The list of the GUI server's computer is asked for only when the user goes to the select.
 */
export class VsilConnectElement extends Part {
  static override observedAttributes = ['network'];

  protected draw(client: Front, root: HTMLElement, alive: () => boolean): void {
    const network = this.getAttribute('network');
    if (!network) {
      throw new Error(
        `<${this.localName}> needs the attribute network="<name>" (a can or ethernet network of the configuration file)`,
      );
    }
    const select = h('select', {}, new Option(NONE_TEXT, 'none'));
    const notes = h('ul', { id: `vsil-connect-notes-${++ids}`, class: 'vsil-why' });
    const findings = h('ul', { id: `vsil-connect-findings-${ids}`, class: 'vsil-findings' });
    select.setAttribute('aria-describedby', `${notes.id} ${findings.id}`);
    root.append(select, notes, findings);
    setData(this, 'network', network);
    this.own(() => {
      setData(this, 'network', null);
      setData(this, 'chosen', null);
    });

    const file = () => client.status.get().file ?? undefined;
    const chosen = () => pageIntent.get().connect?.[network] ?? 'none';
    const say = (list: HTMLElement, lines: readonly string[]) =>
      list.replaceChildren(...lines.map((line) => h('li', {}, line)));

    /** Shows the choice of the page intent; a value that the list does not have is shown as written. */
    const show = () => {
      const value = chosen();
      if (![...select.options].some((o) => o.value === value)) {
        select.add(new Option(value, value));
      }
      select.value = value;
      setData(this, 'chosen', value);
    };

    let listing: Promise<void> | null = null;
    const list = (): Promise<void> => {
      if (listing) return listing; // one list at a time: a press and the focus it brings ask once
      select.setAttribute('aria-busy', 'true');
      listing = client
        .connections({ network }, { file: file() })
        .then(
          (entries) => {
            if (!alive()) return;
            setData(this, 'error', null);
            fill(entries);
          },
          (error: unknown) => {
            if (!alive()) return;
            setData(this, 'error', codeOf(error));
            say(notes, [messageOf(error)]);
          },
        )
        .finally(() => {
          listing = null;
          select.removeAttribute('aria-busy');
        });
      return listing;
    };

    /** The targets in the answer's order, under the name of their backend; a backend that has no list is a note. */
    const fill = (entries: readonly ConnectEntry[]) => {
      select.replaceChildren(new Option(NONE_TEXT, 'none'));
      const errors: string[] = [];
      let group = null as HTMLOptGroupElement | null;
      for (const entry of entries) {
        if ('error' in entry) {
          errors.push(entry.backend ? `${entry.backend}: ${entry.error}` : entry.error);
          continue;
        }
        const label = entry.backend ?? '';
        if (group?.label !== label) {
          group = h('optgroup', { label });
          select.append(group);
        }
        const why = entry.usable ? entry.detail : (entry.reason ?? entry.detail);
        const option = new Option(why ? `${entry.label} — ${why}` : entry.label, entry.value);
        option.disabled = !entry.usable;
        group.append(option);
      }
      say(notes, errors);
      // A choice that the new list cannot take goes back to none: the run is not connected to what was not listed
      const value = chosen();
      const offered = [...select.options].find((o) => o.value === value);
      if (value !== 'none' && (!offered || offered.disabled)) write('none');
      else show();
    };

    let checks = 0;
    const check = () => {
      const asked = ++checks;
      client.check(pageIntent.get(), { file: file() }).then(
        (result) => {
          if (!alive() || asked !== checks) return;
          findings.replaceChildren(
            ...findingsFor(result, network).map((f) => {
              const item = h('li', {}, f.message);
              setData(item, 'severity', f.severity);
              setData(item, 'code', f.code);
              return item;
            }),
          );
        },
        (error: unknown) => {
          if (!alive() || asked !== checks) return;
          say(findings, [messageOf(error)]);
          // The server's last list does not have the choice (the file changed, or no list was made): list again
          if (codeOf(error) === 'target-not-listed') void list();
        },
      );
    };

    /** Writes the choice into the page intent (`none`: the network is left out of `connect`). */
    const write = (value: string) => {
      const connect = { ...pageIntent.get().connect };
      if (value === 'none') delete connect[network];
      else connect[network] = value;
      pageIntent.update({ connect: Object.keys(connect).length > 0 ? connect : undefined });
    };

    const choose = (value: string) => {
      write(value);
      if (value === 'none') {
        checks++; // an answer on its way is no longer about this choice
        findings.replaceChildren();
      } else check();
    };

    select.addEventListener('focus', () => void list());
    select.addEventListener('pointerdown', () => void list());
    select.addEventListener('change', () => choose(select.value));
    this.own(pageIntent.subscribe(show));
    show();
  }
}

const countText = (count: number | null) => (count === null ? '-' : String(count));

/** One row of `<vsil-outside>`: each field in a span of its own, in words (not only in colour). */
function row(outside: Outside): HTMLElement {
  const item = h(
    'li',
    {},
    h('span', { class: 'vsil-network' }, outside.network),
    ' → ',
    h('span', { class: 'vsil-target' }, outside.target ?? '-'),
    ': ',
    h(
      'span',
      { class: 'vsil-link' },
      outside.link !== null ? `link ${outside.link}` : `bus ${outside.bus ?? '-'}`,
    ),
    ', ',
    h('span', { class: 'vsil-condition' }, outside.condition),
    outside.reasons.length > 0
      ? h('span', { class: 'vsil-reasons' }, ` (${outside.reasons.join(', ')})`)
      : '',
    ', ',
    h(
      'span',
      { class: 'vsil-counts' },
      `to outside ${countText(outside.to_outside)}, from outside ${countText(outside.from_outside)}`,
    ),
    outside.nothing_received ? h('span', { class: 'vsil-nothing' }, ', nothing received') : '',
  );
  setData(item, 'network', outside.network);
  setData(item, 'condition', outside.condition);
  setData(item, 'nothingReceived', String(outside.nothing_received));
  return item;
}

/**
 * `<vsil-outside>`: the networks that the run connected outside (`client.outside`), one row each. Nothing is shown when
 * there are none (also in a run that a `role: bridge` participant of the file connects).
 */
export class VsilOutsideElement extends Part {
  protected draw(client: Front, root: HTMLElement): void {
    const rows = h('ul');
    root.append(rows);
    this.own(
      watch(client.outside, (outside) => {
        root.hidden = outside.length === 0;
        rows.replaceChildren(...outside.map(row));
      }),
    );
  }
}
