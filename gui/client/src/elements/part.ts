// The base of the parts: a light-DOM element that binds to the page's connection while it is in the document.
import { connect, watch } from '../index';
import type { Front, OperationName } from '../index';
import { reasonText } from './reasons';

let ids = 0;

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
export async function quietly(operation: () => Promise<void>): Promise<void> {
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
  #root: HTMLElement | null = null;

  /** Another connection than the page's (`connect()`). Setting it while the part is in the document binds it again. */
  get client(): Front | null {
    return this.#client;
  }
  set client(value: Front | null) {
    this.#client = value;
    if (this.isConnected) void this.#bind();
  }

  constructor() {
    super();
    this.upgrade('client');
  }

  /**
   * A property that the page set before this class was defined sits on the object itself and hides the accessor:
   * take it off and set it again.
   */
  protected upgrade(name: string): void {
    if (!Object.hasOwn(this, name)) return;
    const value: unknown = Reflect.get(this, name);
    Reflect.deleteProperty(this, name);
    Reflect.set(this, name, value);
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
    void this.#bind();
  }

  disconnectedCallback(): void {
    this.#bindings++;
    this.#clear();
  }

  attributeChangedCallback(_name: string, before: string | null, after: string | null): void {
    if (before !== after && this.isConnected) void this.#bind();
  }

  #clear(): void {
    for (const stop of this.#stops.splice(0)) stop();
    this.#root?.remove();
    this.#root = null;
  }

  async #bind(): Promise<void> {
    this.#clear();
    const binding = ++this.#bindings;
    const alive = () => binding === this.#bindings && this.isConnected;
    try {
      const client = this.#client ?? (await connect());
      if (!alive()) return;
      this.#root = this.appendChild(h('span', { class: 'vsil' }));
      await this.draw(client, this.#root, alive);
    } catch (error) {
      if (!alive()) return;
      const message = error instanceof Error ? error.message : String(error);
      this.#root ??= this.appendChild(h('span', { class: 'vsil' }));
      this.#root.append(h('small', { role: 'alert' }, message));
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
