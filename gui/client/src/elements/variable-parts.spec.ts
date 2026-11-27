import { userEvent } from 'vitest/browser';
import { ActionError } from '../index';
import type { QueuedWrite, WriteResult } from '../index';
import { fakeFront, viewOf } from './fake-front';
import type { FakeFront } from './fake-front';
import { setReasonText } from './index';
import type { WriteOutcome, WriteQueuedInfo } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

async function mount<K extends 'vsil-value' | 'vsil-slider' | 'vsil-write'>(
  tag: K,
  front: FakeFront,
  attributes: Record<string, string>,
): Promise<HTMLElementTagNameMap[K]> {
  const part = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) part.setAttribute(name, value);
  part.client = front;
  document.body.append(part);
  await settle();
  return part;
}

const noteEl = (part: HTMLElement) => part.querySelector<HTMLElement>('.vsil-note')!;
const note = (part: HTMLElement) => noteEl(part).textContent;

afterEach(() => {
  document.body.replaceChildren();
  setReasonText(null);
});

describe('data-availability', () => {
  it('is on every part, follows the variable, and does not disable anything', async () => {
    const front = fakeFront();
    const parts = [
      await mount('vsil-value', front, { variable: 'E.e.a' }),
      await mount('vsil-slider', front, { variable: 'E.e.a' }),
      await mount('vsil-write', front, { variable: 'E.e.a', value: '1' }),
    ];
    const cell = front.variables.get('E.e.a')!.cell;
    expect(parts.map((p) => p.dataset['availability'])).toEqual(['active', 'active', 'active']);
    cell.set(viewOf({ availability: 'inactive', min: 0, max: 10, value: 5 }));
    expect(parts.map((p) => p.dataset['availability'])).toEqual([
      'inactive',
      'inactive',
      'inactive',
    ]);
    expect(parts.map((p) => p.dataset['canWrite'])).toEqual(['true', 'true', 'true']);
    expect(parts[1].querySelector('input')!.disabled).toBe(false);
    expect(parts[2].querySelector('button')!.getAttribute('aria-disabled')).toBe('false');
    expect(parts.some((p) => p.hasAttribute('aria-disabled'))).toBe(false);
    cell.set(viewOf({ availability: 'unknown', min: 0, max: 10, value: 5 }));
    expect(parts.map((p) => p.dataset['availability'])).toEqual(['unknown', 'unknown', 'unknown']);
  });
});

describe('<vsil-value>', () => {
  it('shows the text and the unit, and the stale mark in data-stale', async () => {
    const front = fakeFront();
    const part = await mount('vsil-value', front, { variable: 'Dash.d.speed' });
    const cell = front.variables.get('Dash.d.speed')!.cell;
    expect(noteEl(part).hidden).toBe(true); // empty note stays hidden to avoid layout jitter
    cell.set(viewOf({ value: 42, text: '42', unit: 'km/h', stale: true, note: 'Paused.' }));
    expect(part.querySelector('output')!.textContent).toBe('42 km/h');
    expect(part.dataset['stale']).toBe('true');
    expect(note(part)).toBe('Paused.');
    expect(noteEl(part).hidden).toBe(false);
    part.toggleAttribute('no-unit', true);
    await settle();
    expect(part.querySelector('output')!.textContent).toBe('42');
    cell.set(viewOf({ value: 42, text: '42', unit: 'km/h', stale: false, note: null }));
    expect(note(part)).toBe('');
    expect(noteEl(part).hidden).toBe(true);
  });

  it('shows a bool as the on and off text and in data-on', async () => {
    const front = fakeFront();
    const part = await mount('vsil-value', front, { variable: 'E.e.lost', on: 'LOST', off: 'ok' });
    const cell = front.variables.get('E.e.lost')!.cell;
    cell.set(viewOf({ value: true }));
    expect(part.querySelector('output')!.textContent).toBe('LOST');
    expect(part.dataset['on']).toBe('true');
    cell.set(viewOf({ value: false }));
    expect(part.querySelector('output')!.textContent).toBe('ok');
    expect(part.dataset['on']).toBe('false');
  });

  it('says what is missing when the variable attribute is not there', async () => {
    const part = await mount('vsil-value', fakeFront(), {});
    expect(part.querySelector('[role=alert]')?.textContent).toContain('variable="ECU.variable"');
  });
});

describe('<vsil-slider>', () => {
  it('is named by the label of the page alone, and the value and the note are not in the name', async () => {
    const front = fakeFront();
    const label = document.createElement('label');
    label.append('Throttle ');
    document.body.append(label);
    const part = document.createElement('vsil-slider');
    part.setAttribute('variable', 'Engine.e.throttle');
    part.client = front;
    label.append(part);
    await settle();
    front.variables.get('Engine.e.throttle')!.cell.set(
      viewOf({
        value: 30,
        text: '30',
        unit: '%',
        min: 0,
        max: 100,
        step: 5,
        can_write: false,
        why: { code: 'x', message: 'Not now.' },
      }),
    );
    await expect.element(part.querySelector('input')!).toHaveAccessibleName('Throttle');
    await expect.element(part.querySelector('input')!).toHaveAccessibleDescription('Not now.');
    // the value with its unit reaches a screen reader through the knob itself (the output is hidden from it)
    expect(part.querySelector('input')!.getAttribute('aria-valuetext')).toBe('30 %');
  });

  it('takes range and value from the variable and writes the number when the knob is released', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 30, text: '30', unit: '%', min: 0, max: 100, step: 5 }));
    const input = part.querySelector('input')!;
    expect([input.min, input.max, input.step, input.value]).toEqual(['0', '100', '5', '30']);
    input.value = '60';
    input.dispatchEvent(new Event('input'));
    variable.cell.set(viewOf({ value: 35, text: '35', unit: '%', min: 0, max: 100, step: 5 }));
    expect(input.value).toBe('60'); // the knob in hand does not jump
    input.dispatchEvent(new Event('change'));
    expect(variable.enqueue).toHaveBeenCalledWith(60);
  });

  it('lets go of a knob that was held when the variable becomes unwritable', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const cell = front.variables.get('Engine.e.throttle')!.cell;
    const range = { min: 0, max: 100, step: 1 };
    cell.set(viewOf({ value: 30, text: '30', ...range }));
    const input = part.querySelector('input')!;
    input.value = '60';
    input.dispatchEvent(new Event('input'));
    cell.set(viewOf({ value: 30, text: '30', can_write: false, ...range })); // the run ended under the hand
    cell.set(viewOf({ value: 10, text: '10', ...range }));
    expect(input.value).toBe('10');
  });

  it('is disabled with the reason when it cannot write, hides quiet/empty notes, and shows failures', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const cell = front.variables.get('Engine.e.throttle')!.cell;
    const range = { min: 0, max: 100 };
    cell.set(
      viewOf({ ...range, can_write: false, why: { code: 'ended', message: 'The run has ended.' } }),
    );
    const input = part.querySelector('input')!;
    expect(input.disabled).toBe(true);
    expect(note(part)).toBe('The run has ended.');
    expect(noteEl(part).hidden).toBe(false);
    expect(part.dataset['reason']).toBe('ended');
    expect(document.getElementById(input.getAttribute('aria-describedby')!)).toBe(noteEl(part));
    setReasonText(() => 'Beendet.');
    cell.set(
      viewOf({ ...range, can_write: false, why: { code: 'ended', message: 'The run has ended!' } }),
    );
    expect(note(part)).toBe('Beendet.');
    setReasonText(null);
    // Quiet reason stays hidden visually while remaining accessible and exposing data-reason.
    cell.set(
      viewOf({
        ...range,
        can_write: false,
        why: { code: 'not-running', message: 'No run is going.' },
      }),
    );
    expect(part.dataset['reason']).toBe('not-running');
    expect(note(part)).toBe('No run is going.');
    expect(noteEl(part).hidden).toBe(true);
    await expect.element(input).toHaveAccessibleDescription('No run is going.');
    // Problem overrides quiet/writable state and becomes visible.
    cell.set(viewOf({ ...range, problem: { code: 'no-answer', message: 'Nobody answered.' } }));
    expect(note(part)).toBe('Nobody answered.');
    expect(noteEl(part).hidden).toBe(false);
    expect(part.dataset['canWrite']).toBe('true');
    expect(part.dataset['reason']).toBeUndefined();
    // Writing shows note, then idle hides the empty note cleanly.
    cell.set(viewOf({ ...range, writing: true }));
    expect(note(part)).toBe('writing…');
    expect(noteEl(part).hidden).toBe(false);
    cell.set(viewOf({ ...range, writing: false }));
    expect(note(part)).toBe('');
    expect(noteEl(part).hidden).toBe(true);
    expect(document.getElementById(input.getAttribute('aria-describedby')!)).toBe(noteEl(part));
  });

  it('keeps the knob where it was put while the write is on its way', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    const range = { min: 0, max: 100, step: 1 };
    variable.cell.set(viewOf({ value: 30, text: '30', ...range }));
    const input = part.querySelector('input')!;
    input.value = '60';
    input.dispatchEvent(new Event('input'));
    input.dispatchEvent(new Event('change'));
    variable.cell.set(viewOf({ value: 30, text: '30', writing: true, ...range }));
    expect(input.value).toBe('60');
    variable.cell.set(viewOf({ value: 60, text: '60', ...range }));
    expect(input.value).toBe('60');
  });

  it('follows the variable when the real pointer takes the knob and puts it back', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    const range = { min: 0, max: 100, step: 1 };
    variable.cell.set(viewOf({ value: 50, text: '50', ...range }));
    const input = part.querySelector('input')!;
    input.style.width = '400px';
    const box = input.getBoundingClientRect();
    const knob = { x: box.left + box.width / 2, y: box.top + box.height / 2 }; // the knob of a middle value is in the middle
    await userEvent.pointer([
      { coords: knob, keys: '[MouseLeft>]' },
      { coords: { x: knob.x + 100, y: knob.y } },
    ]);
    expect(Number(input.value)).toBeGreaterThan(50);
    variable.cell.set(viewOf({ value: 40, text: '40', ...range }));
    expect(Number(input.value)).toBeGreaterThan(50); // the knob in hand does not jump
    await userEvent.pointer([{ coords: knob }, { keys: '[/MouseLeft]' }]);
    expect(input.value).toBe('50');
    expect(variable.enqueue).not.toHaveBeenCalled(); // back where it was: the browser gives no `change`
    variable.cell.set(viewOf({ value: 70, text: '70', ...range }));
    expect(input.value).toBe('70');
  });

  it('writes the value that the real pointer leaves the knob at', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 50, text: '50', min: 0, max: 100, step: 1 }));
    const input = part.querySelector('input')!;
    input.style.width = '400px';
    const box = input.getBoundingClientRect();
    const knob = { x: box.left + box.width / 2, y: box.top + box.height / 2 };
    await userEvent.pointer([
      { coords: knob, keys: '[MouseLeft>]' },
      { coords: { x: knob.x + 100, y: knob.y } },
      { keys: '[/MouseLeft]' },
    ]);
    expect(variable.enqueue).toHaveBeenCalledOnce();
    expect(variable.enqueue.mock.calls[0]![0]).toBeGreaterThan(50);
  });

  it('is not usable for a variable without a range, and says so', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.flag' });
    expect(part.querySelector('input')!.disabled).toBe(true);
    expect(part.querySelector('.vsil-note')!.textContent).toContain('no range');
    expect(noteEl(part).hidden).toBe(false);
    expect(part.dataset['reason']).toBe('no-range');
  });

  it('does not leave an unhandled rejection when the write fails and emits vsil-write-result without vsil-write-queued', async () => {
    // Vitest fails the run on an unhandled rejection.
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));
    const err = new ActionError('no-answer', 'x', 'unknown');
    variable.enqueue.mockRejectedValue(err);
    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    part.addEventListener('vsil-write-queued', (e) => queued.push(e.detail));
    part.addEventListener('vsil-write-result', (e) => results.push(e.detail));
    const input = part.querySelector('input')!;
    input.value = '40';
    input.dispatchEvent(new Event('change'));
    await settle();
    await settle();
    expect(queued).toEqual([]);
    expect(results).toHaveLength(1);
    expect(results[0]).toEqual({
      ok: false,
      peer: 'Engine',
      name: 'e.throttle',
      value: 40,
      error: err,
    });
    expect(results[0]!.ok === false && results[0]!.error).toBe(err);
    expect(part.dataset['writeStatus']).toBe('failed');
    // Knob snaps back to the variable's actual value once the failed enqueue settles.
    expect(input.value).toBe('10');
  });

  it('coalesces rapid slider changes while enqueue is in flight without emitting vsil-write-queued for dropped values', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));

    const enq1 = deferred<QueuedWrite>();
    const app1 = deferred<WriteResult>();
    const enq2 = deferred<QueuedWrite>();
    const app2 = deferred<WriteResult>();
    variable.enqueue.mockReturnValueOnce(enq1.promise).mockReturnValueOnce(enq2.promise);

    const log: string[] = [];
    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    part.addEventListener('vsil-write-queued', (e) => {
      expect(e.bubbles).toBe(true);
      expect(e.composed).toBe(true);
      queued.push(e.detail);
      log.push(`queued:${e.detail.seq}:${String(e.detail.value)}:${part.dataset['writeStatus']}`);
    });
    part.addEventListener('vsil-write-result', (e) => {
      expect(e.bubbles).toBe(true);
      expect(e.composed).toBe(true);
      results.push(e.detail);
      log.push(
        `result:${e.detail.seq ?? 'none'}:${String(e.detail.value)}:${e.detail.ok ? 'ok' : 'err'}:${part.dataset['writeStatus']}`,
      );
    });

    const input = part.querySelector('input')!;
    expect(part.dataset['writeStatus']).toBe('idle');

    // First commit enters enqueue(); subsequent commits coalesce into the single pending slot.
    for (const v of ['20', '40', '60', '80']) {
      input.value = v;
      input.dispatchEvent(new Event('change'));
    }
    expect(part.dataset['writeStatus']).toBe('sending');
    expect(variable.enqueue).toHaveBeenCalledTimes(1);
    expect(variable.enqueue).toHaveBeenCalledWith(20);
    expect(queued).toEqual([]);
    expect(results).toEqual([]);

    // First enqueue resolves -> emits vsil-write-queued(seq=1, 20) and immediately flushes coalesced 80 (never 40 or 60).
    enq1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      applied: () => app1.promise,
    });
    await settle();
    expect(variable.enqueue).toHaveBeenCalledTimes(2);
    expect(variable.enqueue).toHaveBeenNthCalledWith(2, 80);
    expect(queued).toEqual([
      { session: 's1', seq: 1, peer: 'Engine', name: 'e.throttle', value: 20 },
    ]);
    expect(part.dataset['writeStatus']).toBe('sending');

    // Second enqueue resolves -> emits vsil-write-queued(seq=2, 80).
    enq2.resolve({
      session: 's1',
      seq: 2,
      peer: 'Engine',
      name: 'e.throttle',
      value: 80,
      applied: () => app2.promise,
    });
    await settle();
    expect(queued).toEqual([
      { session: 's1', seq: 1, peer: 'Engine', name: 'e.throttle', value: 20 },
      { session: 's1', seq: 2, peer: 'Engine', name: 'e.throttle', value: 80 },
    ]);
    expect(part.dataset['writeStatus']).toBe('queued');

    // Resolve applied() for seq 1 then seq 2.
    app1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      appliedAtMs: 120,
      late: false,
    });
    await settle();
    expect(part.dataset['writeStatus']).toBe('queued'); // seq 2 is still awaiting applied()

    app2.resolve({
      session: 's1',
      seq: 2,
      peer: 'Engine',
      name: 'e.throttle',
      value: 80,
      appliedAtMs: 140,
      late: true,
    });
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');
    expect(results).toEqual([
      {
        ok: true,
        session: 's1',
        seq: 1,
        peer: 'Engine',
        name: 'e.throttle',
        value: 20,
        appliedAtMs: 120,
        late: false,
      },
      {
        ok: true,
        session: 's1',
        seq: 2,
        peer: 'Engine',
        name: 'e.throttle',
        value: 80,
        appliedAtMs: 140,
        late: true,
      },
    ]);
    expect(log).toEqual([
      'queued:1:20:queued',
      'queued:2:80:queued',
      'result:1:20:ok:queued',
      'result:2:80:ok:applied',
    ]);
  });

  it('discards coalesced slider value when the in-flight enqueue fails and never emits vsil-write-queued', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));

    const enq1 = deferred<QueuedWrite>();
    variable.enqueue.mockReturnValueOnce(enq1.promise);

    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    part.addEventListener('vsil-write-queued', (e) => queued.push(e.detail));
    part.addEventListener('vsil-write-result', (e) => results.push(e.detail));

    const input = part.querySelector('input')!;
    input.value = '20';
    input.dispatchEvent(new Event('change'));
    input.value = '80';
    input.dispatchEvent(new Event('change'));

    const err = new ActionError('write-busy', 'server busy', 'refused');
    enq1.reject(err);
    await settle();

    expect(variable.enqueue).toHaveBeenCalledTimes(1);
    expect(queued).toEqual([]);
    expect(results).toEqual([
      { ok: false, peer: 'Engine', name: 'e.throttle', value: 20, error: err },
    ]);
  });

  it('emits dropped failure without vsil-write-queued if the variable becomes unwritable before flushing a coalesced slider value', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));

    const enq1 = deferred<QueuedWrite>();
    const app1 = deferred<WriteResult>();
    variable.enqueue.mockReturnValueOnce(enq1.promise);

    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    part.addEventListener('vsil-write-queued', (e) => queued.push(e.detail));
    part.addEventListener('vsil-write-result', (e) => results.push(e.detail));

    const input = part.querySelector('input')!;
    input.value = '20';
    input.dispatchEvent(new Event('change'));
    input.value = '80';
    input.dispatchEvent(new Event('change'));

    variable.cell.set(
      viewOf({
        value: 10,
        text: '10',
        min: 0,
        max: 100,
        step: 1,
        can_write: false,
        why: { code: 'ended', message: 'The run has ended.' },
      }),
    );

    enq1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      applied: () => app1.promise,
    });
    await settle();

    expect(variable.enqueue).toHaveBeenCalledTimes(1);
    expect(queued).toEqual([
      { session: 's1', seq: 1, peer: 'Engine', name: 'e.throttle', value: 20 },
    ]);
    expect(results).toHaveLength(1);
    expect(results[0]!.ok).toBe(false);
    if (!results[0]!.ok) {
      expect(results[0]!.value).toBe(80);
      expect(results[0]!.seq).toBeUndefined();
      expect(results[0]!.error).toBeInstanceOf(ActionError);
      expect(results[0]!.error.code).toBe('ended');
    }

    app1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      appliedAtMs: 50,
      late: false,
    });
    await settle();
    expect(results).toHaveLength(2);
    expect(results[1]!.ok).toBe(true);
    // Latest write (dropped coalesced 80) stays reflected in data-write-status even after older seq 1 applies.
    expect(part.dataset['writeStatus']).toBe('failed');
  });

  it('keeps the slider knob at the queued value while stage 3+4 is pending and preserves latest status on out-of-order settlement', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));

    const enq1 = deferred<QueuedWrite>();
    const app1 = deferred<WriteResult>();
    const enq2 = deferred<QueuedWrite>();
    const app2 = deferred<WriteResult>();
    variable.enqueue.mockReturnValueOnce(enq1.promise).mockReturnValueOnce(enq2.promise);

    const input = part.querySelector('input')!;
    input.value = '20';
    input.dispatchEvent(new Event('change'));
    input.value = '80';
    input.dispatchEvent(new Event('change'));

    enq1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      applied: () => app1.promise,
    });
    await settle();
    enq2.resolve({
      session: 's1',
      seq: 2,
      peer: 'Engine',
      name: 'e.throttle',
      value: 80,
      applied: () => app2.promise,
    });
    await settle();

    // Even when Front does not set v.writing=true, knob holds at 80 while queued.
    expect(part.dataset['writeStatus']).toBe('queued');
    expect(input.value).toBe('80');

    // Newer seq 2 applies first, then older seq 1 fails -> data-write-status stays 'applied'.
    app2.resolve({
      session: 's1',
      seq: 2,
      peer: 'Engine',
      name: 'e.throttle',
      value: 80,
      appliedAtMs: 140,
      late: false,
    });
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');

    app1.reject(new ActionError('late', 'too late', 'refused'));
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');
    // Once all queued writes settle, knob resyncs to v.value.
    expect(input.value).toBe('10');
  });

  it('invalidates old slider binding immediately on re-entrant or same-tick rebind so no stale events or old-variable sends occur', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const v1 = front.variables.get('Engine.e.throttle')!;
    v1.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));

    // 1. Re-entrant rebind inside vsil-write-queued listener with coalesced pending value.
    const enq1 = deferred<QueuedWrite>();
    const app1 = deferred<WriteResult>();
    v1.enqueue.mockReturnValueOnce(enq1.promise);
    const leaked: Event[] = [];
    document.body.addEventListener('vsil-write-result', (e) => leaked.push(e));
    part.addEventListener(
      'vsil-write-queued',
      () => {
        part.variable = 'Engine.e.other';
      },
      { once: true },
    );

    const input = part.querySelector('input')!;
    input.value = '20';
    input.dispatchEvent(new Event('change'));
    input.value = '80';
    input.dispatchEvent(new Event('change'));

    enq1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      applied: () => app1.promise,
    });
    await settle();
    app1.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 20,
      appliedAtMs: 50,
      late: false,
    });
    await settle();

    expect(v1.enqueue).toHaveBeenCalledTimes(1); // coalesced 80 was NOT sent to old variable
    expect(leaked).toEqual([]);
    expect(part.dataset['writeStatus']).toBe('idle');

    // 2. Same-tick enqueue resolution followed synchronously by variable rebind.
    const v2 = front.variables.get('Engine.e.other')!;
    v2.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));
    const enq2 = deferred<QueuedWrite>();
    v2.enqueue.mockReturnValueOnce(enq2.promise);
    const queuedAfter: Event[] = [];
    document.body.addEventListener('vsil-write-queued', (e) => queuedAfter.push(e));

    const input2 = part.querySelector('input')!;
    input2.value = '30';
    input2.dispatchEvent(new Event('change'));
    input2.value = '90';
    input2.dispatchEvent(new Event('change'));
    enq2.resolve({
      session: 's1',
      seq: 2,
      peer: 'Engine',
      name: 'e.other',
      value: 30,
      applied: async () => {
        throw new Error('must not be awaited');
      },
    });
    part.variable = 'Engine.e.third';
    // Synchronous change on old input right after variable change must also be ignored.
    input2.value = '95';
    input2.dispatchEvent(new Event('change'));
    await settle();

    expect(v2.enqueue).toHaveBeenCalledTimes(1);
    expect(queuedAfter).toEqual([]);
    expect(leaked).toEqual([]);
    expect(part.dataset['writeStatus']).toBe('idle');
  });

  it('emits zero events for locally disabled slider changes or after disconnect', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    const variable = front.variables.get('Engine.e.throttle')!;
    const events: Event[] = [];
    document.body.addEventListener('vsil-write-queued', (e) => events.push(e));
    document.body.addEventListener('vsil-write-result', (e) => events.push(e));

    // Locally disabled (no range): no enqueue, no events.
    const input = part.querySelector('input')!;
    input.dispatchEvent(new Event('change'));
    await settle();
    expect(variable.enqueue).not.toHaveBeenCalled();
    expect(events).toEqual([]);

    // Disconnect while enqueue() is in flight with a coalesced value: zero events leaked.
    variable.cell.set(viewOf({ value: 10, text: '10', min: 0, max: 100, step: 1 }));
    const enq = deferred<QueuedWrite>();
    variable.enqueue.mockReturnValueOnce(enq.promise);
    input.value = '30';
    input.dispatchEvent(new Event('change'));
    input.value = '70';
    input.dispatchEvent(new Event('change'));
    part.remove();
    expect(part.dataset['writeStatus']).toBeUndefined();
    enq.resolve({
      session: 's1',
      seq: 1,
      peer: 'Engine',
      name: 'e.throttle',
      value: 30,
      applied: async () => {
        throw new Error('must not be awaited after disconnect');
      },
    });
    await settle();
    expect(variable.enqueue).toHaveBeenCalledTimes(1);
    expect(events).toEqual([]);
  });
});

describe('<vsil-write>', () => {
  it('writes its value, and is not usable when the variable already has it or cannot be written', async () => {
    const front = fakeFront();
    const part = await mount('vsil-write', front, {
      variable: 'E.e.connected',
      value: 'false',
      label: 'Cut',
    });
    const variable = front.variables.get('E.e.connected')!;
    const button = part.querySelector('button')!;
    expect(button.textContent).toBe('Cut');
    variable.cell.set(viewOf({ value: true }));
    button.click();
    expect(variable.enqueue).toHaveBeenCalledWith(false);
    variable.cell.set(viewOf({ value: false }));
    expect(button.getAttribute('aria-disabled')).toBe('true');
    button.click();
    expect(variable.enqueue).toHaveBeenCalledOnce();
    variable.cell.set(
      viewOf({ value: true, can_write: false, why: { code: 'paused', message: 'Paused.' } }),
    );
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(note(part)).toBe('Paused.');
    expect(noteEl(part).hidden).toBe(true); // paused is a QUIET self-evident state
    expect(part.dataset['reason']).toBe('paused');
    await expect.element(button).toHaveAccessibleDescription('Paused.');
  });

  it('describes why it cannot be pressed when the variable already has the value, hidden, and hides empty notes', async () => {
    const front = fakeFront();
    const part = await mount('vsil-write', front, { variable: 'E.e.connected', value: 'false' });
    const cell = front.variables.get('E.e.connected')!.cell;
    cell.set(viewOf({ value: false }));
    const button = part.querySelector('button')!;
    const why = document.getElementById(button.getAttribute('aria-describedby')!)!;
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(part.dataset['reason']).toBe('already');
    expect(why.textContent).toBe('already false');
    expect(why.hidden).toBe(true);
    await expect.element(button).toHaveAccessibleDescription('already false');
    // Writable and idle: empty note stays hidden to avoid empty layout boxes, while aria-describedby stays valid.
    cell.set(viewOf({ value: true }));
    expect(why.textContent).toBe('');
    expect(why.hidden).toBe(true);
    expect(part.dataset['reason']).toBeUndefined();
    expect(document.getElementById(button.getAttribute('aria-describedby')!)).toBe(why);
    // Non-quiet reason or problem becomes visible.
    cell.set(
      viewOf({
        value: true,
        can_write: false,
        why: { code: 'scenario-running', message: 'A scenario is running.' },
      }),
    );
    expect(why.textContent).toBe('A scenario is running.');
    expect(why.hidden).toBe(false);
    expect(part.dataset['reason']).toBe('scenario-running');
  });

  it('reads numbers and texts from the value attribute and names itself without a label', async () => {
    const front = fakeFront();
    const number = await mount('vsil-write', front, { variable: 'E.e.gear', value: '3' });
    expect(number.querySelector('button')!.textContent).toBe('E.e.gear = 3');
    front.variables.get('E.e.gear')!.cell.set(viewOf({ value: 1 }));
    number.querySelector('button')!.click();
    expect(front.variables.get('E.e.gear')!.enqueue).toHaveBeenCalledWith(3);
    const text = await mount('vsil-write', front, { variable: 'E.e.mode', value: 'eco' });
    text.querySelector('button')!.click();
    expect(front.variables.get('E.e.mode')!.enqueue).toHaveBeenCalledWith('eco');
  });

  it('emits bubbling vsil-write-queued before vsil-write-result for success and post-queue failure', async () => {
    const front = fakeFront();
    const container = document.createElement('div');
    document.body.append(container);
    const part = document.createElement('vsil-write');
    part.setAttribute('variable', 'E.e.connected');
    part.setAttribute('value', 'false');
    part.client = front;
    container.append(part);
    await settle();

    const variable = front.variables.get('E.e.connected')!;
    variable.cell.set(viewOf({ value: true }));

    const log: string[] = [];
    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    container.addEventListener('vsil-write-queued', (e) => {
      const ce = e as CustomEvent<WriteQueuedInfo>;
      expect(ce.bubbles).toBe(true);
      expect(ce.composed).toBe(true);
      queued.push(ce.detail);
      log.push(`queued:${ce.detail.seq}:${part.dataset['writeStatus']}`);
    });
    container.addEventListener('vsil-write-result', (e) => {
      const ce = e as CustomEvent<WriteOutcome>;
      expect(ce.bubbles).toBe(true);
      expect(ce.composed).toBe(true);
      results.push(ce.detail);
      log.push(
        `result:${ce.detail.seq ?? 'none'}:${ce.detail.ok ? 'ok' : 'err'}:${part.dataset['writeStatus']}`,
      );
    });

    // 1. Success flow with deferred applied().
    const enq1 = deferred<QueuedWrite>();
    const app1 = deferred<WriteResult>();
    variable.enqueue.mockReturnValueOnce(enq1.promise);
    part.querySelector('button')!.click();
    expect(part.dataset['writeStatus']).toBe('sending');

    enq1.resolve({
      session: 'run-7',
      seq: 11,
      peer: 'E',
      name: 'e.connected',
      value: false,
      applied: () => app1.promise,
    });
    await settle();
    expect(queued).toEqual([
      { session: 'run-7', seq: 11, peer: 'E', name: 'e.connected', value: false },
    ]);
    expect(results).toEqual([]);
    expect(part.dataset['writeStatus']).toBe('queued');

    app1.resolve({
      session: 'run-7',
      seq: 11,
      peer: 'E',
      name: 'e.connected',
      value: false,
      appliedAtMs: 250,
      late: false,
    });
    await settle();
    expect(results).toEqual([
      {
        ok: true,
        session: 'run-7',
        seq: 11,
        peer: 'E',
        name: 'e.connected',
        value: false,
        appliedAtMs: 250,
        late: false,
      },
    ]);
    expect(part.dataset['writeStatus']).toBe('applied');

    // 2. Post-queue failure: vsil-write-queued fires, then vsil-write-result fires with ok:false, seq, and concrete ActionError.
    const appErr = new ActionError('rejected', 'ecu rejected write', 'refused');
    variable.enqueue.mockResolvedValueOnce({
      session: 'run-7',
      seq: 12,
      peer: 'E',
      name: 'e.connected',
      value: false,
      applied: async () => {
        throw appErr;
      },
    });
    part.querySelector('button')!.click();
    await settle();
    expect(queued).toHaveLength(2);
    expect(results).toHaveLength(2);
    expect(results[1]).toEqual({
      ok: false,
      peer: 'E',
      name: 'e.connected',
      value: false,
      error: appErr,
      seq: 12,
    });
    expect(results[1]!.ok === false && results[1]!.error).toBe(appErr);
    expect(log).toEqual([
      'queued:11:queued',
      'result:11:ok:applied',
      'queued:12:queued',
      'result:12:err:failed',
    ]);
  });

  it('never emits vsil-write-queued on pre-queue failure or disabled clicks, and leaks zero events after disconnect or rebind', async () => {
    const front = fakeFront();
    const part = await mount('vsil-write', front, { variable: 'E.e.connected', value: 'false' });
    const variable = front.variables.get('E.e.connected')!;
    const queued: WriteQueuedInfo[] = [];
    const results: WriteOutcome[] = [];
    document.body.addEventListener('vsil-write-queued', (e) =>
      queued.push((e as CustomEvent<WriteQueuedInfo>).detail),
    );
    document.body.addEventListener('vsil-write-result', (e) =>
      results.push((e as CustomEvent<WriteOutcome>).detail),
    );

    // 1. Locally disabled click -> zero events.
    variable.cell.set(viewOf({ value: false }));
    part.querySelector('button')!.click();
    await settle();
    expect(variable.enqueue).not.toHaveBeenCalled();
    expect(queued).toEqual([]);
    expect(results).toEqual([]);

    // 2. Pre-queue failure -> zero vsil-write-queued, one vsil-write-result(ok:false, no seq).
    variable.cell.set(viewOf({ value: true }));
    const preErr = new ActionError('write-busy', 'busy', 'refused');
    variable.enqueue.mockRejectedValueOnce(preErr);
    part.querySelector('button')!.click();
    await settle();
    expect(queued).toEqual([]);
    expect(results).toEqual([
      { ok: false, peer: 'E', name: 'e.connected', value: false, error: preErr },
    ]);

    // 3. Re-bind while applied() is in flight -> zero events from old binding.
    const appOld = deferred<WriteResult>();
    variable.enqueue.mockResolvedValueOnce({
      session: 's1',
      seq: 20,
      peer: 'E',
      name: 'e.connected',
      value: false,
      applied: () => appOld.promise,
    });
    part.querySelector('button')!.click();
    await settle();
    expect(queued).toHaveLength(1);
    part.variable = 'E.e.other';
    await settle();
    appOld.reject(new ActionError('cancelled', 'cancelled', 'refused'));
    await settle();
    expect(results).toHaveLength(1); // old rejection did not leak after rebind

    // 4. Synchronous disconnect inside vsil-write-queued listener -> zero vsil-write-result leaked.
    const otherVar = front.variables.get('E.e.other')!;
    otherVar.cell.set(viewOf({ value: true }));
    const appAfterDisconnect = deferred<WriteResult>();
    otherVar.enqueue.mockResolvedValueOnce({
      session: 's1',
      seq: 21,
      peer: 'E',
      name: 'e.other',
      value: false,
      applied: () => appAfterDisconnect.promise,
    });
    part.addEventListener('vsil-write-queued', () => part.remove(), { once: true });
    part.querySelector('button')!.click();
    await settle();
    expect(queued).toHaveLength(2);
    expect(part.isConnected).toBe(false);
    appAfterDisconnect.resolve({
      session: 's1',
      seq: 21,
      peer: 'E',
      name: 'e.other',
      value: false,
      appliedAtMs: 99,
      late: false,
    });
    await settle();
    expect(results).toHaveLength(1);
  });

  it('tracks latest-write data-write-status across out-of-order settlements, slow pre-queue failures, and run resets', async () => {
    const front = fakeFront();
    front.cells.status.patch({ run: 'running' });
    const part = await mount('vsil-write', front, { variable: 'E.e.cmd', value: '1' });
    const variable = front.variables.get('E.e.cmd')!;
    variable.cell.set(viewOf({ value: 0 }));

    // 1. Out-of-order applied(): seq 2 applied then seq 1 failed -> stays 'applied'.
    const app1 = deferred<WriteResult>();
    const app2 = deferred<WriteResult>();
    variable.enqueue
      .mockResolvedValueOnce({
        session: 's1',
        seq: 1,
        peer: 'E',
        name: 'e.cmd',
        value: 1,
        applied: () => app1.promise,
      })
      .mockResolvedValueOnce({
        session: 's1',
        seq: 2,
        peer: 'E',
        name: 'e.cmd',
        value: 1,
        applied: () => app2.promise,
      });
    const button = part.querySelector('button')!;
    button.click();
    button.click();
    await settle();
    expect(part.dataset['writeStatus']).toBe('queued');
    app2.resolve({
      session: 's1',
      seq: 2,
      peer: 'E',
      name: 'e.cmd',
      value: 1,
      appliedAtMs: 20,
      late: false,
    });
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');
    app1.reject(new ActionError('late', 'old failed', 'refused'));
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');

    // 2. Reverse out-of-order: seq 4 fails then seq 3 applies -> stays 'failed'.
    const app3 = deferred<WriteResult>();
    const app4 = deferred<WriteResult>();
    variable.enqueue
      .mockResolvedValueOnce({
        session: 's1',
        seq: 3,
        peer: 'E',
        name: 'e.cmd',
        value: 1,
        applied: () => app3.promise,
      })
      .mockResolvedValueOnce({
        session: 's1',
        seq: 4,
        peer: 'E',
        name: 'e.cmd',
        value: 1,
        applied: () => app4.promise,
      });
    button.click();
    button.click();
    await settle();
    app4.reject(new ActionError('rejected', 'newer failed', 'refused'));
    await settle();
    expect(part.dataset['writeStatus']).toBe('failed');
    app3.resolve({
      session: 's1',
      seq: 3,
      peer: 'E',
      name: 'e.cmd',
      value: 1,
      appliedAtMs: 30,
      late: false,
    });
    await settle();
    expect(part.dataset['writeStatus']).toBe('failed');

    // 3. Slow pre-queue failure after a newer write already applied -> stays 'applied'.
    const slowEnq = deferred<QueuedWrite>();
    variable.enqueue.mockReturnValueOnce(slowEnq.promise).mockResolvedValueOnce({
      session: 's1',
      seq: 5,
      peer: 'E',
      name: 'e.cmd',
      value: 1,
      applied: async () => ({
        session: 's1',
        seq: 5,
        peer: 'E',
        name: 'e.cmd',
        value: 1,
        appliedAtMs: 50,
        late: false,
      }),
    });
    button.click();
    button.click();
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');
    slowEnq.reject(new ActionError('write-timeout', 'slow failed', 'unknown'));
    await settle();
    expect(part.dataset['writeStatus']).toBe('applied');

    // 4. Cross-run reset via client.status run transition.
    variable.enqueue.mockRejectedValueOnce(new ActionError('write-busy', 'busy', 'refused'));
    button.click();
    await settle();
    expect(part.dataset['writeStatus']).toBe('failed');
    front.cells.status.patch({ run: 'ended' });
    expect(part.dataset['writeStatus']).toBe('idle');
    front.cells.status.patch({ run: 'running' });
    expect(part.dataset['writeStatus']).toBe('idle');

    // 5. Cross-run reset via VariableView condition transition (even without client.status change).
    variable.enqueue.mockRejectedValueOnce(new ActionError('write-busy', 'busy', 'refused'));
    button.click();
    await settle();
    expect(part.dataset['writeStatus']).toBe('failed');
    variable.cell.set(viewOf({ value: 0, condition: { kind: 'unavailable', status: 'off' } }));
    expect(part.dataset['writeStatus']).toBe('idle');
    variable.cell.set(viewOf({ value: 0 }));
    expect(part.dataset['writeStatus']).toBe('idle');

    // 6. External problem clear resets stale local 'failed' status.
    variable.cell.set(viewOf({ value: 0, problem: { code: 'no-answer', message: 'timeout' } }));
    variable.enqueue.mockRejectedValueOnce(new ActionError('no-answer', 'timeout', 'unknown'));
    button.click();
    await settle();
    expect(part.dataset['writeStatus']).toBe('failed');
    variable.cell.set(viewOf({ value: 0, problem: null }));
    expect(part.dataset['writeStatus']).toBe('idle');
  });

  it('drops same-tick settlements and clicks immediately when variable or client is rebound', async () => {
    const front1 = fakeFront();
    const front2 = fakeFront();
    const part = await mount('vsil-write', front1, { variable: 'E.e.cmd', value: '1' });
    const v1 = front1.variables.get('E.e.cmd')!;
    v1.cell.set(viewOf({ value: 0 }));

    const leaked: Event[] = [];
    document.body.addEventListener('vsil-write-result', (e) => leaked.push(e));

    // Same-tick applied() resolution right before variable rebind + synchronous old button click.
    const app1 = deferred<WriteResult>();
    v1.enqueue.mockResolvedValueOnce({
      session: 's1',
      seq: 1,
      peer: 'E',
      name: 'e.cmd',
      value: 1,
      applied: () => app1.promise,
    });
    const oldButton = part.querySelector('button')!;
    oldButton.click();
    await settle();
    app1.resolve({
      session: 's1',
      seq: 1,
      peer: 'E',
      name: 'e.cmd',
      value: 1,
      appliedAtMs: 11,
      late: false,
    });
    part.variable = 'E.e.other';
    oldButton.click();
    await settle();
    expect(v1.enqueue).toHaveBeenCalledTimes(1);
    expect(leaked).toEqual([]);
    expect(part.dataset['writeStatus']).toBe('idle');

    // Same-tick applied() resolution right before client swap.
    const vOther = front1.variables.get('E.e.other')!;
    vOther.cell.set(viewOf({ value: 0 }));
    const app2 = deferred<WriteResult>();
    vOther.enqueue.mockResolvedValueOnce({
      session: 's1',
      seq: 2,
      peer: 'E',
      name: 'e.other',
      value: 1,
      applied: () => app2.promise,
    });
    part.querySelector('button')!.click();
    await settle();
    app2.resolve({
      session: 's1',
      seq: 2,
      peer: 'E',
      name: 'e.other',
      value: 1,
      appliedAtMs: 22,
      late: false,
    });
    part.client = front2;
    await settle();
    expect(leaked).toEqual([]);
    expect(part.dataset['writeStatus']).toBe('idle');
  });
});
