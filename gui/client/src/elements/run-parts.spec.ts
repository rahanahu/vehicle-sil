import { userEvent } from 'vitest/browser';
import { ActionError } from '../index';
import { fakeFront, ok, refused } from './fake-front';
import type { FakeFront } from './fake-front';
import { pageIntent, setReasonText } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

async function mount<K extends 'vsil-status' | 'vsil-run-controls' | 'vsil-time-controls'>(
  tag: K,
  front: FakeFront,
  attributes: Record<string, string> = {},
): Promise<HTMLElementTagNameMap[K]> {
  const part = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) part.setAttribute(name, value);
  part.client = front;
  document.body.append(part);
  await settle();
  return part;
}

const buttons = (part: HTMLElement) => [...part.querySelectorAll('button')];
const button = (part: HTMLElement, name: string) =>
  buttons(part).find((b) => b.textContent === name)!;
const described = (b: HTMLElement) => document.getElementById(b.getAttribute('aria-describedby')!)!;

afterEach(() => {
  document.body.replaceChildren();
  setReasonText(null);
  pageIntent.update({ scenario: '', variant: '' });
});

describe('<vsil-status>', () => {
  it('shows the link, the run, the time, a late mark and the problem, and puts them in data-*', async () => {
    const front = fakeFront();
    const part = await mount('vsil-status', front);
    expect(part.textContent).toContain('ready');
    front.cells.status.patch({ run: 'running' });
    front.cells.clock.patch({ sim_ms: 120, pacing: 'late' });
    front.cells.status.patch({ problem: { code: 'x', message: 'It broke.' } });
    expect(part.dataset).toMatchObject({ link: 'online', run: 'running', pacing: 'late' });
    expect(part.textContent).toContain('120 ms (behind real time)');
    expect(part.querySelector('[role=alert]')?.textContent).toBe('It broke.');
    front.cells.status.patch({
      problem: null,
      run: 'ended',
      result: { end: 'completed', scenario: null, ok: true, message: 'Finished.' },
    });
    expect(part.textContent).toContain('Finished.');
    front.cells.status.patch({ link: 'offline' });
    expect(part.textContent).toContain('offline');
  });

  it('writes the problem with setReasonText, and the owner sentence when it returns null', async () => {
    const front = fakeFront();
    front.cells.status.patch({ problem: { code: 'busy', message: 'Wait.' } });
    setReasonText((reason) => (reason.code === 'busy' ? 'Warten.' : null));
    const part = await mount('vsil-status', front);
    expect(part.querySelector('[role=alert]')?.textContent).toBe('Warten.');
    setReasonText(() => undefined);
    front.cells.status.patch({ problem: { code: 'busy', message: 'Wait.' } });
    front.cells.status.patch({ problem: { code: 'busy', message: 'Wait!' } });
    expect(part.querySelector('[role=alert]')?.textContent).toBe('Wait!');
  });
});

describe('the buttons', () => {
  let front: FakeFront;
  beforeEach(() => (front = fakeFront()));

  it('does not call the operation when the control is not ok, and says why next to the button', async () => {
    front.cells.controls.patch({ start: refused('running', 'A run is going.') });
    const part = await mount('vsil-run-controls', front);
    const start = button(part, 'Start');
    expect(start.getAttribute('aria-disabled')).toBe('true');
    expect(start.dataset['reason']).toBe('running');
    expect(described(start).textContent).toBe('A run is going.');
    start.click();
    await settle();
    expect(front.spies.start).not.toHaveBeenCalled();
    front.cells.controls.patch({ start: ok() });
    expect(start.getAttribute('aria-disabled')).toBe('false');
    expect(described(start).textContent).toBe('');
    start.click();
    expect(front.spies.start).toHaveBeenCalledOnce();
  });

  it('shows the text from setReasonText, and the original when it returns null', async () => {
    front.cells.controls.patch({ start: refused('running', 'A run is going.') });
    setReasonText((reason) => (reason.code === 'running' ? 'Läuft schon.' : null));
    const part = await mount('vsil-run-controls', front);
    expect(described(button(part, 'Start')).textContent).toBe('Läuft schon.');
    setReasonText((reason) => (reason.code === 'other' ? 'x' : null));
    front.cells.controls.patch({ start: refused('running', 'A run is going!') });
    expect(described(button(part, 'Start')).textContent).toBe('A run is going!');
  });

  it('shows the force stop only while it is ok', async () => {
    const part = await mount('vsil-run-controls', front);
    const force = part.querySelector<HTMLElement>('.vsil-force')!;
    expect(force.hidden).toBe(true);
    front.cells.controls.patch({ force_stop: ok() });
    expect(force.hidden).toBe(false);
    force.querySelector('button')!.click();
    expect(front.spies.forceStop).toHaveBeenCalledOnce();
  });

  it('names the buttons by attributes', async () => {
    const part = await mount('vsil-run-controls', front, {
      'label-start': 'Los',
      'label-stop': 'Halt',
    });
    expect(buttons(part).map((b) => b.textContent)).toEqual([
      'Los',
      'Halt',
      'Stop without waiting',
    ]);
  });

  it('starts with the page selection, and the intent property wins', async () => {
    const part = await mount('vsil-run-controls', front);
    pageIntent.update({ scenario: 'scenarios/a.yaml', variant: 'v1' });
    button(part, 'Start').click();
    expect(front.spies.start).toHaveBeenLastCalledWith(
      { scenario: 'scenarios/a.yaml', variant: 'v1' },
      { file: 'simulation.yaml' },
    );
    part.intent = { scenario: 'scenarios/b.yaml' };
    button(part, 'Start').click();
    expect(front.spies.start).toHaveBeenLastCalledWith(
      { scenario: 'scenarios/b.yaml', variant: 'v1' },
      { file: 'simulation.yaml' },
    );
  });

  it('starts the file that the status names, and no file when there is none', async () => {
    front.cells.status.patch({ file: 'other/b.yaml' });
    const part = await mount('vsil-run-controls', front);
    button(part, 'Start').click();
    expect(front.spies.start).toHaveBeenLastCalledWith({}, { file: 'other/b.yaml' });
    front.cells.status.patch({ file: null });
    button(part, 'Start').click();
    expect(front.spies.start).toHaveBeenLastCalledWith({}, undefined);
  });

  it('puts a reason that only says there is nothing to do in the description, hidden', async () => {
    front.cells.controls.patch({ stop: refused('not-running', 'No run is going.') });
    const part = await mount('vsil-run-controls', front);
    const why = described(button(part, 'Stop'));
    expect(why.textContent).toBe('No run is going.');
    expect(why.hidden).toBe(true);
    front.cells.controls.patch({ stop: refused('busy', 'Another action is on its way.') });
    expect(why.hidden).toBe(false);
  });

  it('keeps a refused button focusable, and its reason is its accessible description even when hidden', async () => {
    front.cells.controls.patch({ stop: refused('not-running', 'No run is going.') });
    const part = await mount('vsil-run-controls', front);
    const stop = button(part, 'Stop');
    for (let i = 0; i < 5 && document.activeElement !== stop; i++) await userEvent.tab(); // a disabled button would be skipped
    expect(document.activeElement).toBe(stop);
    expect(stop.getAttribute('aria-disabled')).toBe('true');
    await expect.element(stop).toHaveAccessibleDescription('No run is going.');
    await userEvent.keyboard('{Enter}');
    expect(front.spies.stop).not.toHaveBeenCalled();
  });

  it('does not leave an unhandled rejection when an operation fails', async () => {
    // Vitest fails the run on an unhandled rejection.
    front.spies.stop.mockRejectedValue(
      new ActionError('busy', 'Another action is on its way.', 'refused'),
    );
    const part = await mount('vsil-run-controls', front);
    button(part, 'Stop').click();
    await settle();
    await settle();
    expect(front.spies.stop).toHaveBeenCalledOnce();
  });
});

describe('<vsil-time-controls>', () => {
  it('calls the time operations with the inputs, and the speed with the chosen value', async () => {
    const front = fakeFront();
    const part = await mount('vsil-time-controls', front, { speeds: '1 max', 'step-ms': '25' });
    const [step, until] = [...part.querySelectorAll('input')];
    expect(step!.value).toBe('25');
    button(part, 'Pause').click();
    button(part, 'Resume').click();
    button(part, 'Step').click();
    until!.value = '900';
    button(part, 'Run until').click();
    expect(front.spies.pause).toHaveBeenCalledOnce();
    expect(front.spies.resume).toHaveBeenCalledOnce();
    expect(front.spies.step).toHaveBeenCalledWith(25);
    expect(front.spies.runUntil).toHaveBeenCalledWith(900);
    const speed = part.querySelector('select')!;
    expect([...speed.options].map((o) => o.value)).toEqual(['', '1', 'max']);
    speed.value = 'max';
    speed.dispatchEvent(new Event('change'));
    expect(front.spies.setSpeed).toHaveBeenCalledWith('max');
    front.cells.clock.patch({ speed: 3 });
    expect(speed.value).toBe('3');
  });

  it('marks a button that cannot be used and does not call it', async () => {
    const front = fakeFront();
    front.cells.controls.patch({ pause: refused('paused', 'Already paused.') });
    const part = await mount('vsil-time-controls', front);
    button(part, 'Pause').click();
    expect(front.spies.pause).not.toHaveBeenCalled();
    expect(described(button(part, 'Pause')).textContent).toBe('Already paused.');
  });

  it('describes the speed like a button, and goes back to the clock when it is refused', async () => {
    const front = fakeFront();
    front.cells.controls.patch({
      speed: { ...refused('not-running', 'No run is going.'), min: 0.1, max: 100 },
    });
    const part = await mount('vsil-time-controls', front, { speeds: '1 max' });
    const speed = part.querySelector('select')!;
    expect(speed.getAttribute('aria-disabled')).toBe('true');
    expect(speed.dataset['reason']).toBe('not-running');
    expect(described(speed).textContent).toBe('No run is going.');
    expect(speed.value).toBe(''); // no speed known yet
    speed.value = '1';
    speed.dispatchEvent(new Event('change'));
    expect(front.spies.setSpeed).not.toHaveBeenCalled();
    expect(speed.value).toBe('');
    front.cells.controls.patch({ speed: { ...ok(), min: 0.1, max: 100 } });
    front.cells.clock.patch({ speed: 'max' });
    front.spies.setSpeed.mockRejectedValue(new Error('refused'));
    speed.value = '1';
    speed.dispatchEvent(new Event('change'));
    await settle();
    expect(front.spies.setSpeed).toHaveBeenCalledWith(1);
    expect(speed.value).toBe('max');
  });

  it('has the labels of the buttons as attributes', async () => {
    const part = await mount('vsil-time-controls', fakeFront(), {
      'label-pause': 'P',
      'label-resume': 'R',
      'label-step': 'S',
      'label-run-until': 'U',
    });
    expect(buttons(part).map((b) => b.textContent)).toEqual(['P', 'R', 'S', 'U']);
  });
});
