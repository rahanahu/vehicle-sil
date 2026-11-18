import { userEvent } from 'vitest/browser';
import { ActionError } from '../index';
import { fakeFront, viewOf } from './fake-front';
import type { FakeFront } from './fake-front';
import { setReasonText } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

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

const note = (part: HTMLElement) => part.querySelector('.vsil-note')!.textContent;

afterEach(() => {
  document.body.replaceChildren();
  setReasonText(null);
});

describe('<vsil-value>', () => {
  it('shows the text and the unit, and the stale mark in data-stale', async () => {
    const front = fakeFront();
    const part = await mount('vsil-value', front, { variable: 'Dash.d.speed' });
    const cell = front.variables.get('Dash.d.speed')!.cell;
    cell.set(viewOf({ value: 42, text: '42', unit: 'km/h', stale: true, note: 'Paused.' }));
    expect(part.querySelector('output')!.textContent).toBe('42 km/h');
    expect(part.dataset['stale']).toBe('true');
    expect(note(part)).toBe('Paused.');
    part.toggleAttribute('no-unit', true);
    await settle();
    expect(part.querySelector('output')!.textContent).toBe('42');
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
    expect(variable.write).toHaveBeenCalledWith(60);
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

  it('is disabled with the reason when it cannot write, and shows the failed write', async () => {
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
    expect(document.getElementById(input.getAttribute('aria-describedby')!)).toBe(
      part.querySelector('.vsil-note'),
    );
    setReasonText(() => 'Beendet.');
    cell.set(
      viewOf({ ...range, can_write: false, why: { code: 'ended', message: 'The run has ended!' } }),
    );
    expect(note(part)).toBe('Beendet.');
    setReasonText(null);
    cell.set(viewOf({ ...range, problem: { code: 'no-answer', message: 'Nobody answered.' } }));
    expect(note(part)).toBe('Nobody answered.');
    expect(part.dataset['canWrite']).toBe('true');
    cell.set(viewOf({ ...range, writing: true }));
    expect(note(part)).toBe('writing…');
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
    expect(variable.write).not.toHaveBeenCalled(); // back where it was: the browser gives no `change`
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
    expect(variable.write).toHaveBeenCalledOnce();
    expect(variable.write.mock.calls[0]![0]).toBeGreaterThan(50);
  });

  it('is not usable for a variable without a range, and says so', async () => {
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.flag' });
    expect(part.querySelector('input')!.disabled).toBe(true);
    expect(part.querySelector('.vsil-note')!.textContent).toContain('no range');
  });

  it('does not leave an unhandled rejection when the write fails', async () => {
    // Vitest fails the run on an unhandled rejection.
    const front = fakeFront();
    const part = await mount('vsil-slider', front, { variable: 'Engine.e.throttle' });
    front.variables
      .get('Engine.e.throttle')!
      .write.mockRejectedValue(new ActionError('no-answer', 'x', 'unknown'));
    part.querySelector('input')!.dispatchEvent(new Event('change'));
    await settle();
    await settle();
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
    expect(variable.write).toHaveBeenCalledWith(false);
    variable.cell.set(viewOf({ value: false }));
    expect(button.getAttribute('aria-disabled')).toBe('true');
    button.click();
    expect(variable.write).toHaveBeenCalledOnce();
    variable.cell.set(
      viewOf({ value: true, can_write: false, why: { code: 'paused', message: 'Paused.' } }),
    );
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(note(part)).toBe('Paused.');
  });

  it('describes why it cannot be pressed when the variable already has the value, hidden', async () => {
    const front = fakeFront();
    const part = await mount('vsil-write', front, { variable: 'E.e.connected', value: 'false' });
    front.variables.get('E.e.connected')!.cell.set(viewOf({ value: false }));
    const button = part.querySelector('button')!;
    const why = document.getElementById(button.getAttribute('aria-describedby')!)!;
    expect(button.getAttribute('aria-disabled')).toBe('true');
    expect(why.textContent).toBe('already false');
    expect(why.hidden).toBe(true);
    front.variables.get('E.e.connected')!.cell.set(viewOf({ value: true }));
    expect(why.hidden).toBe(false);
  });

  it('reads numbers and texts from the value attribute and names itself without a label', async () => {
    const front = fakeFront();
    const number = await mount('vsil-write', front, { variable: 'E.e.gear', value: '3' });
    expect(number.querySelector('button')!.textContent).toBe('E.e.gear = 3');
    front.variables.get('E.e.gear')!.cell.set(viewOf({ value: 1 }));
    number.querySelector('button')!.click();
    expect(front.variables.get('E.e.gear')!.write).toHaveBeenCalledWith(3);
    const text = await mount('vsil-write', front, { variable: 'E.e.mode', value: 'eco' });
    text.querySelector('button')!.click();
    expect(front.variables.get('E.e.mode')!.write).toHaveBeenCalledWith('eco');
  });
});
