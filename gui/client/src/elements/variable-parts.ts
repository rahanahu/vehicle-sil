// The parts of one variable: show it, set it with a slider, write one value with a button.
import { ActionError, DROPPED_MESSAGE, watch } from '../index';
import type {
  Front,
  FrontVariable,
  QueuedWrite,
  RunState,
  ValueCondition,
  VariableRef,
  VariableView,
  WriteStatus,
} from '../index';
import type { VsilWriteEventMap, WriteOutcome, WriteQueuedInfo } from './contracts';
import { Part, h, isQuietReason, setData } from './part';
import { reasonText } from './reasons';

let noteIds = 0;

/** The line under a variable part: why it cannot be written, or why the last write failed. */
const writeNote = (v: VariableView): string => {
  if (v.problem) return reasonText(v.problem);
  if (!v.can_write && v.why) return reasonText(v.why);
  return v.writing ? 'writing…' : '';
};

/** Splits `"Peer.variable"` at the first dot, matching `refOf` for failure metadata when `enqueue` fails before queuing. */
function parseFallbackRef(raw: string): VariableRef {
  const dot = raw.indexOf('.');
  return dot > 0 ? { peer: raw.slice(0, dot), name: raw.slice(dot + 1) } : { peer: raw, name: raw };
}

/** Preserves concrete `ActionError` instances and wraps any non-`ActionError` rejection cleanly. */
function toActionError(error: unknown): ActionError {
  if (error instanceof ActionError) return error;
  const message = error instanceof Error ? error.message : String(error);
  const obj = error !== null && typeof error === 'object' ? error : null;
  const code: unknown = obj ? Reflect.get(obj, 'code') : undefined;
  const kind: unknown = obj ? Reflect.get(obj, 'kind') : undefined;
  const outcome: unknown = obj ? Reflect.get(obj, 'outcome') : undefined;
  const resolvedCode =
    typeof code === 'string' && code !== ''
      ? code
      : typeof kind === 'string' && kind !== ''
        ? kind
        : 'error';
  const resolvedOutcome =
    outcome === 'unknown' || kind === 'no-answer' || kind === 'closed' ? 'unknown' : 'refused';
  return new ActionError(resolvedCode, message, resolvedOutcome);
}

/** Reads the kind string of `VariableView.write_status` (handles both `WriteStatus` objects and legacy string fakes). */
function writeStatusKind(status: WriteStatus | string | undefined): string {
  if (typeof status === 'string') return status;
  if (status && typeof status === 'object' && typeof status.kind === 'string') return status.kind;
  return 'idle';
}

/** Reads the kind string of `VariableView.condition` (handles both `ValueCondition` objects and legacy string fakes). */
function conditionKindOf(condition: ValueCondition | string | undefined): string {
  if (typeof condition === 'string') return condition;
  if (condition && typeof condition === 'object' && typeof condition.kind === 'string')
    return condition.kind;
  return 'live';
}

const RUN_BOUNDARY_STATES = new Set<RunState>(['none', 'starting', 'ended', 'crashed']);
const RUN_BOUNDARY_CONDITIONS = new Set(['unavailable', 'missing', 'waiting', 'ended']);
const RUN_BOUNDARY_REASONS = new Set(['ended', 'not-ready', 'not-running']);

type LocalWriteStatus = 'idle' | 'sending' | 'queued' | 'applied' | 'failed';

/** Per-binding write lifecycle tracker following the latest initiated write (matching `VariableBinder.shown`). */
interface WriteLifecycleTracker {
  enqueueStarted: () => number;
  enqueueQueued: (token: number) => void;
  enqueueFailed: (token: number) => void;
  appliedSucceeded: (token: number) => void;
  appliedFailed: (token: number) => void;
  dropped: () => void;
  hasQueued: () => boolean;
}

/** Per-binding context passed from `VariablePart.draw()` to subclasses. */
interface VariableDrawContext {
  readonly variable: FrontVariable;
  readonly root: HTMLElement;
  readonly note: HTMLElement;
  readonly alive: () => boolean;
  readonly fallbackRef: VariableRef;
  readonly trackLifecycle: WriteLifecycleTracker;
}

/** A part that shows or sets the variable of its `variable` attribute (`"ECU.variable"`). */
abstract class VariablePart extends Part {
  static override observedAttributes = ['variable'];

  /** Whether the part writes (so that it says why it cannot and tracks `data-write-status`). */
  protected readonly writes: boolean = false;

  get variable(): string {
    return this.getAttribute('variable') ?? '';
  }
  set variable(value: string | null) {
    this.reflectNonEmpty('variable', value);
  }

  constructor() {
    super();
    this.upgrade('variable');
  }

  /** Builds the controls once; returns what to do with each new view. */
  protected abstract build(ctx: VariableDrawContext): (view: VariableView) => void;

  protected draw(client: Front, root: HTMLElement, alive: () => boolean): void {
    const name = this.getAttribute('variable');
    if (!name) {
      throw new Error(
        `<${this.localName}> needs the attribute variable="ECU.variable" (the ECU's name, a dot and the variable's name)`,
      );
    }
    const variable = client.variable(name);
    const note = h('small', { id: `vsil-note-${++noteIds}`, class: 'vsil-note' });
    let lastView: VariableView | null = null;
    let nextToken = 0;
    let shownToken = 0;
    let shownStatus: LocalWriteStatus = 'idle';
    let queuedCount = 0;

    const syncWriteStatus = (): void => {
      if (!this.writes || !alive()) return;
      let status: string;
      if (shownToken > 0) {
        status = shownStatus;
      } else {
        const viewKind = lastView ? writeStatusKind(lastView.write_status) : 'idle';
        if (viewKind !== 'idle') status = viewKind;
        else if (lastView?.writing) status = 'sending';
        else if (lastView?.problem) status = 'failed';
        else status = 'idle';
      }
      setData(this, 'writeStatus', status);
    };

    const resetLocalStatus = (): void => {
      shownToken = 0;
      shownStatus = 'idle';
      queuedCount = 0;
      syncWriteStatus();
    };

    const show = this.build({
      variable,
      root,
      note,
      alive,
      fallbackRef: parseFallbackRef(name),
      trackLifecycle: {
        enqueueStarted: () => {
          const token = ++nextToken;
          shownToken = token;
          shownStatus = 'sending';
          syncWriteStatus();
          return token;
        },
        enqueueQueued: (token: number) => {
          queuedCount++;
          if (token === shownToken) shownStatus = 'queued';
          syncWriteStatus();
        },
        enqueueFailed: (token: number) => {
          if (token === shownToken) shownStatus = 'failed';
          syncWriteStatus();
        },
        appliedSucceeded: (token: number) => {
          queuedCount = Math.max(0, queuedCount - 1);
          if (token === shownToken) shownStatus = 'applied';
          syncWriteStatus();
        },
        appliedFailed: (token: number) => {
          queuedCount = Math.max(0, queuedCount - 1);
          if (token === shownToken) shownStatus = 'failed';
          syncWriteStatus();
        },
        dropped: () => {
          shownToken = ++nextToken;
          shownStatus = 'failed';
          syncWriteStatus();
        },
        hasQueued: () => queuedCount > 0,
      },
    });
    root.append(' ', note);
    this.own(() => {
      setData(this, 'stale', null);
      setData(this, 'canWrite', null);
      setData(this, 'availability', null);
      setData(this, 'reason', null);
      setData(this, 'writeStatus', null);
    });
    if (this.writes && client.status) {
      let prevRun: RunState | null = null;
      let prevFile: string | null | undefined;
      this.own(
        watch(client.status, (s) => {
          if (prevRun !== null) {
            const runChanged =
              s.run !== prevRun &&
              (RUN_BOUNDARY_STATES.has(s.run) || RUN_BOUNDARY_STATES.has(prevRun));
            const fileChanged = s.file !== prevFile;
            if (runChanged || fileChanged) resetLocalStatus();
          }
          prevRun = s.run;
          prevFile = s.file;
        }),
      );
    }
    let prevConditionKind: string | null = null;
    let prevWhyCode: string | null = null;
    let prevCanWrite: boolean | null = null;
    let prevHasProblem = false;
    this.own(
      watch(variable, (v) => {
        const condKind = conditionKindOf(v.condition);
        const whyCode = v.why?.code ?? null;
        if (this.writes && lastView !== null) {
          const enteredBoundary =
            condKind !== prevConditionKind && RUN_BOUNDARY_CONDITIONS.has(condKind);
          const leftBoundary =
            prevConditionKind !== null &&
            RUN_BOUNDARY_CONDITIONS.has(prevConditionKind) &&
            !RUN_BOUNDARY_CONDITIONS.has(condKind);
          const reopenedFromEnded =
            prevCanWrite === false &&
            v.can_write &&
            prevWhyCode !== null &&
            RUN_BOUNDARY_REASONS.has(prevWhyCode);
          const problemClearedExternally =
            prevHasProblem && v.problem === null && shownStatus === 'failed';
          if (enteredBoundary || leftBoundary || reopenedFromEnded || problemClearedExternally) {
            shownToken = 0;
            shownStatus = 'idle';
            if (enteredBoundary || leftBoundary || reopenedFromEnded) queuedCount = 0;
          }
        }
        lastView = v;
        prevConditionKind = condKind;
        prevWhyCode = whyCode;
        prevCanWrite = v.can_write;
        prevHasProblem = v.problem !== null;
        setData(this, 'stale', String(v.stale));
        setData(this, 'canWrite', String(v.can_write));
        setData(this, 'availability', v.availability);
        if (this.writes) {
          const text = writeNote(v);
          note.textContent = text;
          setData(this, 'reason', !v.can_write && v.why ? v.why.code : null);
          note.hidden =
            text === '' ||
            (!v.problem && !v.can_write && v.why !== null && isQuietReason(v.why.code));
          syncWriteStatus();
        }
        show(v);
      }),
    );
  }
}

/** Shared base for variable parts that write (`<vsil-slider>`, `<vsil-write>`) and emit write lifecycle DOM events. */
abstract class WritingVariablePart extends VariablePart {
  protected override readonly writes = true;

  override addEventListener<K extends keyof VsilWriteEventMap>(
    type: K,
    listener: (this: this, ev: VsilWriteEventMap[K]) => unknown,
    options?: boolean | AddEventListenerOptions,
  ): void;
  override addEventListener<K extends keyof HTMLElementEventMap>(
    type: K,
    listener: (this: HTMLElement, ev: HTMLElementEventMap[K]) => unknown,
    options?: boolean | AddEventListenerOptions,
  ): void;
  override addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | AddEventListenerOptions,
  ): void;
  override addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | AddEventListenerOptions,
  ): void {
    super.addEventListener(type, listener, options);
  }

  override removeEventListener<K extends keyof VsilWriteEventMap>(
    type: K,
    listener: (this: this, ev: VsilWriteEventMap[K]) => unknown,
    options?: boolean | EventListenerOptions,
  ): void;
  override removeEventListener<K extends keyof HTMLElementEventMap>(
    type: K,
    listener: (this: HTMLElement, ev: HTMLElementEventMap[K]) => unknown,
    options?: boolean | EventListenerOptions,
  ): void;
  override removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | EventListenerOptions,
  ): void;
  override removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | EventListenerOptions,
  ): void {
    super.removeEventListener(type, listener, options);
  }

  /** Dispatches a bubbling, composed write lifecycle event only while this binding is still alive and connected. */
  protected emitWriteEvent<K extends keyof VsilWriteEventMap>(
    type: K,
    detail: VsilWriteEventMap[K]['detail'],
    alive: () => boolean,
  ): void {
    if (!alive() || !this.isConnected) return;
    this.dispatchEvent(new CustomEvent(type, { detail, bubbles: true, composed: true }));
  }

  /** Awaits stage 3+4 (`queued.applied()`) and emits `vsil-write-result` without ever leaking unhandled rejections. */
  protected async awaitApplied(
    queued: QueuedWrite,
    token: number,
    ctx: Pick<VariableDrawContext, 'alive' | 'trackLifecycle'>,
    onSettled?: () => void,
  ): Promise<void> {
    try {
      const result = await queued.applied();
      if (!ctx.alive()) return;
      ctx.trackLifecycle.appliedSucceeded(token);
      onSettled?.();
      const detail: WriteOutcome = { ok: true, ...result };
      this.emitWriteEvent('vsil-write-result', detail, ctx.alive);
    } catch (error: unknown) {
      if (!ctx.alive()) return;
      ctx.trackLifecycle.appliedFailed(token);
      onSettled?.();
      const detail: WriteOutcome = {
        ok: false,
        peer: queued.peer,
        name: queued.name,
        value: queued.value,
        error: toActionError(error),
        seq: queued.seq,
      };
      this.emitWriteEvent('vsil-write-result', detail, ctx.alive);
    }
  }
}

/** `<vsil-value variable="ECU.variable" [on="LOST" off="ok"] [no-unit]>`: the value and its unit; a bool shows as the `on` and `off` text. */
export class VsilValueElement extends VariablePart {
  static override observedAttributes = ['variable', 'on', 'off', 'no-unit'];

  get on(): string {
    return this.getAttribute('on') ?? '';
  }
  set on(value: string | null) {
    this.reflectString('on', value);
  }

  get off(): string {
    return this.getAttribute('off') ?? '';
  }
  set off(value: string | null) {
    this.reflectString('off', value);
  }

  get noUnit(): boolean {
    const raw = this.getAttribute('no-unit');
    return raw !== null && raw.toLowerCase() !== 'false';
  }
  set noUnit(value: boolean) {
    this.toggleAttribute('no-unit', Boolean(value));
  }

  constructor() {
    super();
    this.upgrade('on', 'off', 'noUnit');
  }

  protected build({ root, note }: VariableDrawContext) {
    const output = root.appendChild(h('output'));
    return (v: VariableView) => {
      if (typeof v.value === 'boolean') {
        setData(this, 'on', String(v.value));
        output.textContent = v.value ? this.text('on', 'on') : this.text('off', 'off');
      } else {
        setData(this, 'on', null);
        const unit = this.noUnit || !v.unit ? '' : ` ${v.unit}`;
        output.textContent = v.value === null ? '-' : `${v.text}${unit}`;
      }
      const text = v.note ?? '';
      note.textContent = text;
      note.hidden = text === '';
    };
  }
}

/** `<vsil-slider variable="ECU.variable">`: a number in the variable's range; it is written when the knob is released. */
export class VsilSliderElement extends WritingVariablePart {
  protected build(ctx: VariableDrawContext) {
    const { variable, root, note, alive, fallbackRef, trackLifecycle } = ctx;
    const input = h('input', { type: 'range', 'aria-describedby': note.id });
    // Inside a page's <label> the name is the label's text: the range input already says its value, and the note
    // reaches it as its description (aria-describedby works on a hidden element), so neither goes into the name.
    const output = h('output', { 'aria-hidden': 'true' });
    note.setAttribute('aria-hidden', 'true');
    let dragging = false;
    // Single-slot coalescing (matching VariableBinder): while an enqueue() is in flight, newer slider commits overwrite
    // `pending` so superseded intermediate values never reach the ECU and never emit `vsil-write-queued`.
    let sending = false;
    let pending: { value: number } | null = null;
    const syncKnob = (v: VariableView): void => {
      const busy = v.writing || sending || pending !== null || trackLifecycle.hasQueued();
      // while a write is on its way the knob stays where it was put (the view still has the value that was read)
      if (!dragging && !busy && typeof v.value === 'number') input.value = String(v.value);
      // the output is hidden from a screen reader (it would become part of a label's name): the knob says the value itself
      if (v.value === null || dragging || busy) input.removeAttribute('aria-valuetext');
      else input.setAttribute('aria-valuetext', output.textContent ?? '');
    };

    const send = (value: number): void => {
      if (!alive()) return;
      sending = true;
      const token = trackLifecycle.enqueueStarted();
      void (async () => {
        let queued: QueuedWrite;
        try {
          queued = await variable.enqueue(value);
        } catch (error: unknown) {
          sending = false;
          pending = null;
          if (!alive()) return;
          trackLifecycle.enqueueFailed(token);
          syncKnob(variable.get());
          this.emitWriteEvent(
            'vsil-write-result',
            {
              ok: false,
              peer: fallbackRef.peer,
              name: fallbackRef.name,
              value,
              error: toActionError(error),
            },
            alive,
          );
          return;
        }
        if (!alive()) {
          sending = false;
          pending = null;
          return;
        }
        trackLifecycle.enqueueQueued(token);
        const info: WriteQueuedInfo = {
          session: queued.session,
          seq: queued.seq,
          peer: queued.peer,
          name: queued.name,
          value: queued.value,
        };
        // Keep `sending` true while dispatching `vsil-write-queued` so re-entrant changes coalesce into `pending`.
        this.emitWriteEvent('vsil-write-queued', info, alive);
        if (!alive()) {
          sending = false;
          pending = null;
          return;
        }
        // Start waiting for stage 3+4 independently so the coalesced next value can enqueue immediately.
        void this.awaitApplied(queued, token, ctx, () => syncKnob(variable.get()));
        sending = false;
        const next = pending;
        pending = null;
        if (next !== null) {
          const current = variable.get();
          const currentRanged = current.min !== null && current.max !== null;
          if (!input.disabled && current.can_write && currentRanged) {
            send(next.value);
          } else {
            trackLifecycle.dropped();
            syncKnob(current);
            const why = current.why;
            this.emitWriteEvent(
              'vsil-write-result',
              {
                ok: false,
                peer: fallbackRef.peer,
                name: fallbackRef.name,
                value: next.value,
                error: new ActionError(
                  why?.code ?? 'dropped',
                  why?.message ?? DROPPED_MESSAGE,
                  'refused',
                ),
              },
              alive,
            );
          }
        } else {
          syncKnob(variable.get());
        }
      })();
    };

    input.addEventListener('input', () => {
      dragging = true;
      input.removeAttribute('aria-valuetext'); // while the knob moves it says its own position, not the value read
    });
    input.addEventListener('change', () => {
      dragging = false;
      if (!alive() || input.disabled || !variable.get().can_write) return;
      const nextValue = Number(input.value);
      if (!Number.isFinite(nextValue)) return;
      if (sending) {
        pending = { value: nextValue };
        return;
      }
      send(nextValue);
    });
    // A knob let go of where it was picked up gives no `change`: it follows the variable again.
    for (const name of ['pointerup', 'pointercancel', 'keyup', 'blur'])
      input.addEventListener(name, () => (dragging = false));
    root.append(input, ' ', output);
    return (v: VariableView) => {
      const ranged = v.min !== null && v.max !== null;
      input.disabled = !v.can_write || !ranged;
      if (!v.can_write) dragging = false; // a knob that cannot be released must not stay held
      if (ranged) {
        input.min = String(v.min);
        input.max = String(v.max);
        input.step = v.step === null ? 'any' : String(v.step);
      } else if (v.can_write && !v.problem) {
        note.textContent = 'this variable has no range to set';
        note.hidden = false;
        setData(this, 'reason', 'no-range');
      }
      output.textContent = v.value === null ? '-' : `${v.text}${v.unit ? ` ${v.unit}` : ''}`;
      syncKnob(v);
    };
  }
}

/** The value of a `value` attribute: JSON (a number, true, false), else the text as it is. */
const target = (text: string | null): number | boolean | string => {
  if (text === null) return true;
  try {
    const value: unknown = JSON.parse(text);
    if (typeof value === 'number' || typeof value === 'boolean') return value;
  } catch {
    // not JSON: it is a text
  }
  return text;
};

/** `<vsil-write variable="ECU.variable" value="false" label="Cut">`: writes one value when pressed (not when it already is that value). */
export class VsilWriteElement extends WritingVariablePart {
  static override observedAttributes = ['variable', 'value', 'label'];

  get value(): string | number | boolean | null {
    const raw = this.getAttribute('value');
    return raw === null ? null : target(raw);
  }
  set value(value: string | number | boolean | null) {
    if (value === null || value === undefined) this.removeAttribute('value');
    else this.setAttribute('value', String(value));
  }

  get label(): string {
    return this.getAttribute('label') ?? '';
  }
  set label(value: string | null) {
    this.reflectString('label', value);
  }

  constructor() {
    super();
    this.upgrade('value', 'label');
  }

  protected build(ctx: VariableDrawContext) {
    const { variable, root, note, alive, fallbackRef, trackLifecycle } = ctx;
    const value = target(this.getAttribute('value'));
    const button = h(
      'button',
      { type: 'button', 'aria-describedby': note.id },
      this.text('label', `${this.getAttribute('variable')} = ${String(value)}`),
    );
    button.addEventListener('click', () => {
      if (button.getAttribute('aria-disabled') === 'true' || !alive()) return;
      const token = trackLifecycle.enqueueStarted();
      void (async () => {
        let queued: QueuedWrite;
        try {
          queued = await variable.enqueue(value);
        } catch (error: unknown) {
          if (!alive()) return;
          trackLifecycle.enqueueFailed(token);
          this.emitWriteEvent(
            'vsil-write-result',
            {
              ok: false,
              peer: fallbackRef.peer,
              name: fallbackRef.name,
              value,
              error: toActionError(error),
            },
            alive,
          );
          return;
        }
        if (!alive()) return;
        trackLifecycle.enqueueQueued(token);
        const info: WriteQueuedInfo = {
          session: queued.session,
          seq: queued.seq,
          peer: queued.peer,
          name: queued.name,
          value: queued.value,
        };
        this.emitWriteEvent('vsil-write-queued', info, alive);
        if (!alive()) return;
        await this.awaitApplied(queued, token, ctx);
      })();
    });
    root.append(button);
    return (v: VariableView) => {
      const already = v.can_write && v.value === value;
      button.setAttribute('aria-disabled', String(!v.can_write || already));
      if (already && !v.problem) {
        // said to a screen reader (aria-describedby), not shown: the page shows the value itself
        note.textContent = `already ${String(value)}`;
        note.hidden = true;
        setData(this, 'reason', 'already');
      }
    };
  }
}
