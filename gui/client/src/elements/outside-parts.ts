// The parts of connecting a network outside the simulation: choose where a network of the next run is connected, and
// show the networks that the run connected. They have no rules: the sentences are the launcher's, the bridges' and the
// GUI server's, and whether a target can be chosen is the launcher's `usable`.
import { watch } from '../index';
import type {
  CheckResult,
  ConnectEntry,
  ConnectTarget,
  Finding,
  Front,
  Intent,
  Outside,
} from '../index';
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

/** How long the part lets the page intent settle before it checks it (ms): a run of changes is checked once. */
const CHECK_DELAY_MS = 200;

/** Keys that pass the select by, or close it: they do not open the choice. */
const PASSING_KEYS = new Set(['Tab', 'Shift', 'Escape', 'Control', 'Alt', 'Meta']);

/** The check on its way for each connection: the parts that check the same intent of the same file share it. */
const checking = new WeakMap<Front, { key: string; promise: Promise<CheckResult> }>();

const keyOf = (intent: Intent, file: string | undefined) => JSON.stringify([file ?? null, intent]);

function sharedCheck(
  client: Front,
  intent: Intent,
  file: string | undefined,
): Promise<CheckResult> {
  const key = keyOf(intent, file);
  const going = checking.get(client);
  if (going?.key === key) return going.promise;
  const entry = { key, promise: client.check(intent, { file }) };
  checking.set(client, entry);
  const done = () => {
    if (checking.get(client) === entry) checking.delete(client);
  };
  entry.promise.then(done, done);
  return entry.promise;
}

/**
 * The lines of a check that are about connecting `network`: its own `connect-*` and `outside-*` findings; the
 * `connect-*` findings of the other networks that `intent` connects, after their name (a finding about two networks
 * names one of them); and, when the check says that this network makes the run keep real time (`time_lock.because`),
 * the `outside-*` findings of the whole run.
 */
function findingsFor(
  result: CheckResult,
  intent: Intent,
  network: string,
): { text: string; finding: Finding }[] {
  const lock: unknown = result.raw?.['time_lock'];
  const because: unknown =
    typeof lock === 'object' && lock !== null ? Reflect.get(lock, 'because') : null;
  const locks = Array.isArray(because) && because.includes(network);
  const others = new Set(
    Object.entries(intent.connect ?? {})
      .filter(([name, target]) => name !== network && target !== 'none')
      .map(([name]) => name),
  );
  const lines: { text: string; finding: Finding }[] = [];
  for (const finding of result.findings) {
    if (finding.network === network && prefixed(finding, ['connect-', 'outside-'])) {
      lines.push({ text: finding.message, finding });
    } else if (
      finding.network !== null &&
      others.has(finding.network) &&
      prefixed(finding, ['connect-'])
    ) {
      lines.push({ text: `${finding.network}: ${finding.message}`, finding });
    } else if (locks && finding.network === null && prefixed(finding, ['outside-'])) {
      lines.push({ text: finding.message, finding });
    }
  }
  return lines;
}

/** The text of a target: its label, then its detail and, for one that cannot be used, the reason. */
function targetText(target: ConnectTarget): string {
  const about = [target.detail, target.usable ? '' : (target.reason ?? '')].filter((t) => t !== '');
  return about.length > 0 ? `${target.label} — ${about.join('; ')}` : target.label;
}

/**
 * `<vsil-connect network="N">`: where network N of the next run is connected outside the simulation. The first choice
 * is SIL Kit only (`none`). The list of the GUI server's computer is asked for only when the user opens the select (a
 * press, or a key other than Tab, Escape and the modifiers). When the part leaves the page, or `network` or `file`
 * changes, N leaves `pageIntent.connect`: no run is connected by a choice that is not on the page. `file` names the
 * configuration file that the page starts (default: the start-up file, as `run.start`).
 */
export class VsilConnectElement extends Part {
  static override observedAttributes = ['network', 'file'];

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

    const file = () => this.getAttribute('file') ?? client.status.get().file ?? undefined;
    const chosen = () => pageIntent.get().connect?.[network] ?? 'none';
    const say = (list: HTMLElement, lines: readonly string[]) =>
      list.replaceChildren(...lines.map((line) => h('li', {}, line)));

    /** The lines of the notes: what the last list could not give, and a choice that went back to SIL Kit only. */
    let listNotes: string[] = [];
    let reverted: string | null = null;
    const sayNotes = () => say(notes, reverted === null ? listNotes : [...listNotes, reverted]);

    /** Writes the choice into the page intent (`none`: the network is left out of `connect`). */
    const write = (value: string) => {
      const connect = { ...pageIntent.get().connect };
      if (value === 'none') delete connect[network];
      else connect[network] = value;
      pageIntent.update({ connect: Object.keys(connect).length > 0 ? connect : undefined });
    };

    /** The option of a choice that the list does not have, shown as written (the page put it into the intent). */
    let written: HTMLOptionElement | null = null;
    const show = () => {
      const value = chosen();
      if (written && written.value !== value) {
        written.remove();
        written = null;
      }
      if (![...select.options].some((o) => o.value === value)) {
        written = new Option(value, value);
        select.add(written);
      }
      select.value = value;
      setData(this, 'chosen', value);
    };

    /** The user has opened the choice: a refused check may list the network again (once). */
    let opened = false;
    let relisted = false;
    let listing: Promise<void> | null = null;
    const list = (): Promise<void> => {
      if (listing) return listing; // one list at a time: a press and the keys that follow ask once
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
            listNotes = [messageOf(error)];
            sayNotes();
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
      written = null;
      listNotes = [];
      let group = null as HTMLOptGroupElement | null;
      for (const entry of entries) {
        if ('error' in entry) {
          listNotes.push(entry.backend ? `${entry.backend}: ${entry.error}` : entry.error);
          continue;
        }
        const label = entry.backend ?? '';
        if (group?.label !== label) {
          group = h('optgroup', { label });
          select.append(group);
        }
        const option = new Option(targetText(entry), entry.value);
        option.disabled = !entry.usable;
        group.append(option);
      }
      // A choice that the new list cannot take goes back to none, and says so: the run is not connected to what was
      // not listed
      const value = chosen();
      const offered = entries.find((e): e is ConnectTarget => !('error' in e) && e.value === value);
      if (value !== 'none' && !offered?.usable) {
        reverted = offered
          ? `'${value}' cannot be used${offered.reason ? ` (${offered.reason})` : ''}: back to SIL Kit only`
          : `'${value}' is not in the list of this computer: back to SIL Kit only`;
        setData(this, 'reverted', value);
        sayNotes();
        write('none');
      } else {
        sayNotes();
        show();
      }
    };

    let checks = 0;
    let checked: string | null = null;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const check = () => {
      const intent = pageIntent.get();
      const asked = ++checks;
      checked = keyOf(intent, file());
      sharedCheck(client, intent, file()).then(
        (result) => {
          if (!alive() || asked !== checks) return;
          relisted = false;
          findings.replaceChildren(
            ...findingsFor(result, intent, network).map(({ text, finding }) => {
              const item = h('li', {}, text);
              setData(item, 'severity', finding.severity);
              setData(item, 'code', finding.code);
              return item;
            }),
          );
        },
        (error: unknown) => {
          if (!alive() || asked !== checks) return;
          checked = null; // the same intent is checked again at its next change
          say(findings, [messageOf(error)]);
          // The server's last list does not have the choice (the file changed, or no list was made). After the user
          // has opened the choice, list again once, and check again if the choice is still there
          if (codeOf(error) !== 'target-not-listed' || !opened || relisted) return;
          relisted = true;
          void list().then(() => {
            if (alive() && asked === checks && chosen() !== 'none') check();
          });
        },
      );
    };

    /** The page intent changed: check it again after it settles, while this network has a choice. */
    const changed = () => {
      show();
      clearTimeout(timer);
      if (chosen() === 'none') {
        checks++; // an answer on its way is no longer about this choice
        checked = null;
        findings.replaceChildren();
        return;
      }
      if (keyOf(pageIntent.get(), file()) === checked) return;
      timer = setTimeout(check, CHECK_DELAY_MS);
    };

    const open = () => {
      opened = true;
      void list();
    };
    select.addEventListener('pointerdown', open);
    select.addEventListener('keydown', (event) => {
      if (!PASSING_KEYS.has(event.key)) open();
    });
    select.addEventListener('change', () => {
      reverted = null;
      setData(this, 'reverted', null);
      relisted = false;
      sayNotes();
      write(select.value);
    });
    this.own(pageIntent.subscribe(changed));
    this.own(() => {
      clearTimeout(timer);
      checks++;
      setData(this, 'network', null);
      setData(this, 'chosen', null);
      setData(this, 'reverted', null);
      if (chosen() !== 'none') write('none'); // a choice that is not on the page connects nothing
    });
    changed();
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
      outside.link !== null
        ? `link ${outside.link}`
        : outside.bus !== null
          ? `bus ${outside.bus}`
          : '-',
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
