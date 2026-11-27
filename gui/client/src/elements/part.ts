// The base of the parts: a light-DOM element that binds to the page's connection while it is in the document.
import { connect, watch } from '../index';
import type { Front, OperationName } from '../index';
import { reasonText } from './reasons';

let ids = 0;

/** Parts whose bind failed on the page's connection and that are still in a document: one of them tells it. */
const failed = new Map<Part, (speaking: boolean) => void>();
let reconciling = false;

/** The part that shows the sentence of a failed connection in `document`: the first `<vsil-status>`, else the first part. */
function speaker(document: Document): Part | null {
  let chosen: Part | null = null;
  const rank = (part: Part) => (part.localName === 'vsil-status' ? 0 : 1);
  for (const part of failed.keys()) {
    if (part.ownerDocument !== document) continue;
    if (
      chosen === null ||
      rank(part) < rank(chosen) ||
      (rank(part) === rank(chosen) &&
        !!(part.compareDocumentPosition(chosen) & Node.DOCUMENT_POSITION_FOLLOWING))
    ) {
      chosen = part;
    }
  }
  return chosen;
}

/** Decides again, after the parts that fail together have all failed, which failed part shows the sentence. */
function reconcile(): void {
  if (reconciling) return;
  reconciling = true;
  queueMicrotask(() => {
    reconciling = false;
    const speakers = new Set<Part>();
    for (const part of failed.keys()) {
      const chosen = speaker(part.ownerDocument);
      if (chosen) speakers.add(chosen);
    }
    for (const [part, speak] of failed) speak(speakers.has(part));
  });
}

/** Creates an element with text and attributes. */
export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attributes: Record<string, string> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) element.setAttribute(name, value);
  element.append(...children);
  return element;
}

/** Sets or removes a data attribute. */
export function setData(element: HTMLElement, name: string, value: string | null): void {
  if (value === null) delete element.dataset[name];
  else element.dataset[name] = value;
}

/** Runs an operation and drops its failure: `status.problem` (or the variable's `problem`) shows it. */
export async function quietly(operation: () => Promise<unknown>): Promise<void> {
  try {
    await operation();
  } catch {
    // shown by the part that watches status or the variable
  }
}

/** Reasons that only say there is nothing to do yet: their sentence is in the description, hidden. */
const QUIET = new Set(['not-running', 'offline']);

/**
 * The base of the parts. Subclasses draw into `root` in `draw()` and register what to stop with `own()`.
 *
 * - The part draws into one `<span class="vsil">` that it adds; children the page put in the element stay.
 * - It binds when it enters the document, unbinds when it leaves, and binds again when one of
 *   `observedAttributes` changes or `client` is set.
 */
export abstract class Part extends HTMLElement {
  static observedAttributes: string[] = [];

  #stops: (() => void)[] = [];
  #client: Front | null = null;
  #bindings = 0;
  #bindScheduled = false;
  #root: HTMLElement | null = null;
  #sentence: string | null = null;

  /** Another connection than the page's (`connect()`). Setting it while the part is in the document binds it again. */
  get client(): Front | null {
    return this.#client;
  }
  set client(value: Front | null) {
    const next = value ?? null;
    if (next === this.#client) return;
    this.#client = next;
    if (this.isConnected) this.#scheduleBind();
  }

  constructor() {
    super();
    this.upgrade('client');
  }

  /**
   * A property that the page set before this class was defined sits on the object itself and hides the accessor:
   * take it off and set it again.
   */
  protected upgrade(...names: string[]): void {
    for (const name of names) {
      if (!Object.hasOwn(this, name)) continue;
      const value: unknown = Reflect.get(this, name);
      Reflect.deleteProperty(this, name);
      Reflect.set(this, name, value);
    }
  }

  /** Reflects a string attribute; `null` or `undefined` removes it while empty string is kept. */
  protected reflectString(name: string, value: string | null | undefined): void {
    if (value === null || value === undefined) this.removeAttribute(name);
    else this.setAttribute(name, String(value));
  }

  /** Reflects a non-empty string attribute; `null`, `undefined` or `""` removes it. */
  protected reflectNonEmpty(name: string, value: string | null | undefined): void {
    if (value === null || value === undefined || value === '') this.removeAttribute(name);
    else this.setAttribute(name, String(value));
  }

  /** Draws and subscribes. `alive()` turns false when the part has left the document or was bound again. */
  protected abstract draw(
    client: Front,
    root: HTMLElement,
    alive: () => boolean,
  ): void | Promise<void>;

  /** Stops `stop` when the part is unbound. */
  protected own(stop: () => void): void {
    this.#stops.push(stop);
  }

  /** The attribute `name`, or `fallback` when it is not there. */
  protected text(name: string, fallback: string): string {
    return this.getAttribute(name) ?? fallback;
  }

  connectedCallback(): void {
    this.#scheduleBind();
  }

  disconnectedCallback(): void {
    this.#bindings++;
    this.#bindScheduled = false;
    this.#clear();
  }

  attributeChangedCallback(_name: string, before: string | null, after: string | null): void {
    if (before !== after && this.isConnected) this.#scheduleBind();
  }

  #scheduleBind(): void {
    if (this.#bindScheduled || !this.isConnected) return;
    this.#bindScheduled = true;
    const generation = this.#bindings;
    queueMicrotask(() => {
      if (this.#bindings !== generation) return;
      this.#bindScheduled = false;
      if (!this.isConnected) return;
      void this.#bind();
    });
  }

  /** Shows the sentence of the failed connection (`speaking`) or not; the mark `data-error` stays either way. */
  #speak = (speaking: boolean): void => {
    if (this.#sentence === null) return;
    const shown = this.#root?.querySelector(':scope > [role=alert]') ?? null;
    if (speaking && !shown) {
      this.#root ??= this.appendChild(h('span', { class: 'vsil' }));
      this.#root.append(h('small', { role: 'alert' }, this.#sentence));
    } else if (!speaking && shown) {
      this.#root?.remove();
      this.#root = null;
    }
  };

  #fail(error: unknown, shared: boolean): void {
    const message = error instanceof Error ? error.message : String(error);
    const code: unknown = Reflect.get(Object(error), 'code');
    setData(this, 'error', typeof code === 'string' && code !== '' ? code : 'error');
    if (shared) {
      this.#sentence = message;
      failed.set(this, this.#speak);
      reconcile();
      return;
    }
    this.#root ??= this.appendChild(h('span', { class: 'vsil' }));
    this.#root.append(h('small', { role: 'alert' }, message));
  }

  #clear(): void {
    this.#sentence = null;
    setData(this, 'error', null);
    if (failed.delete(this)) reconcile();
    for (const stop of this.#stops.splice(0)) stop();
    this.#root?.remove();
    this.#root = null;
  }

  async #bind(): Promise<void> {
    this.#clear();
    const binding = ++this.#bindings;
    const alive = () => binding === this.#bindings && this.isConnected;
    let shared = false;
    try {
      let client = this.#client;
      if (!client) {
        shared = true;
        client = await connect();
        shared = false;
      }
      if (!alive()) return;
      this.#root = this.appendChild(h('span', { class: 'vsil' }));
      await this.draw(client, this.#root, alive);
    } catch (error) {
      if (!alive()) return;
      this.#fail(error, shared);
    }
  }

  /**
   * Binds `element` to one control of `client.controls`: `aria-disabled` (the caller ignores input while it is true),
   * `data-reason` and `data-caution` (the codes), and a sentence in an element that describes it (`aria-describedby`;
   * returned, to be put next to `element`). A reason that only says there is nothing to do (`not-running`, `offline`)
   * is in that element too, but hidden: a screen reader reads it, the page does not show it seven times.
   */
  protected describe(client: Front, control: OperationName, element: HTMLElement): HTMLElement {
    const why = h('small', { id: `vsil-why-${++ids}`, class: 'vsil-why' });
    element.setAttribute('aria-describedby', why.id);
    this.own(
      watch(client.controls, (controls) => {
        const { ok, reason, caution } = controls[control];
        element.setAttribute('aria-disabled', String(!ok));
        setData(element, 'reason', reason?.code ?? null);
        setData(element, 'caution', caution?.code ?? null);
        const shown = reason ?? caution;
        why.textContent = shown ? reasonText(shown) : '';
        why.hidden = shown !== null && QUIET.has(shown.code);
      }),
    );
    return why;
  }

  /**
   * A button bound to one control of `client.controls`. When it cannot be used it stays focusable (`aria-disabled`),
   * the reason is in an element that describes it (`aria-describedby`) and its code is in `data-reason`.
   */
  protected button(
    client: Front,
    label: string,
    control: OperationName,
    action: () => Promise<void>,
  ): HTMLElement[] {
    const button = h('button', { type: 'button' }, label);
    const why = this.describe(client, control, button);
    button.addEventListener('click', () => {
      if (button.getAttribute('aria-disabled') !== 'true') void quietly(action);
    });
    return [button, why];
  }
}
