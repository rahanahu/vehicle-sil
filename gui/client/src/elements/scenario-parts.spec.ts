import { fakeFront } from './fake-front';
import type { FakeFront } from './fake-front';
import { pageIntent } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

async function mount<K extends 'vsil-scenario-select' | 'vsil-scenario-result'>(
  tag: K,
  front: FakeFront,
): Promise<HTMLElementTagNameMap[K]> {
  const part = document.createElement(tag);
  part.client = front;
  document.body.append(part);
  await settle();
  return part;
}

afterEach(() => {
  document.body.replaceChildren();
  pageIntent.update({ scenario: '' });
});

describe('<vsil-scenario-select>', () => {
  it('lists the scenarios of the status file and writes the choice to the page intent', async () => {
    const front = fakeFront();
    const part = await mount('vsil-scenario-select', front);
    expect(front.spies.scenarios).toHaveBeenCalledWith('simulation.yaml');
    const select = part.querySelector('select')!;
    expect([...select.options].map((o) => o.textContent)).toEqual(['(no scenario)', 'a.yaml']);
    select.value = 'scenarios/a.yaml';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().scenario).toBe('scenarios/a.yaml');
    select.value = '';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().scenario).toBeUndefined();
  });

  it('reads the list again when the file changes, and is empty without a file', async () => {
    const front = fakeFront();
    const part = await mount('vsil-scenario-select', front);
    front.spies.scenarios.mockResolvedValue({
      file: 'b.yaml',
      directory: 's',
      found: true,
      scenarios: [],
      truncated: false,
    });
    front.cells.status.patch({ file: 'b.yaml' });
    await settle();
    expect(front.spies.scenarios).toHaveBeenLastCalledWith('b.yaml');
    expect(part.querySelectorAll('option')).toHaveLength(1);
    front.cells.status.patch({ file: null });
    await settle();
    expect(part.querySelectorAll('option')).toHaveLength(1);
    front.cells.status.patch({ run: 'running' }); // another field: no new request
    expect(front.spies.scenarios).toHaveBeenCalledTimes(2);
  });

  it('drops a choice that the new list does not have', async () => {
    const front = fakeFront();
    pageIntent.update({ scenario: 'scenarios/gone.yaml' });
    await mount('vsil-scenario-select', front);
    expect(pageIntent.get().scenario).toBeUndefined();
  });
});

describe('<vsil-scenario-result>', () => {
  it('shows the status and the steps, and nothing for a run without a scenario', async () => {
    const front = fakeFront();
    const part = await mount('vsil-scenario-result', front);
    expect(part.querySelector('output')!.textContent).toBe('-');
    front.cells.scenario.set({
      file: 's.yaml',
      name: 's',
      status: 'failed',
      timeline_url: null,
      steps: [{ index: 1, text: 'expect speed', status: 'failed', at_ms: 20, message: 'too slow' }],
    } as never);
    expect(part.dataset['status']).toBe('failed');
    expect(part.querySelector('li')!.textContent).toBe('expect speed — failed at 20 ms: too slow');
    expect(part.querySelector('li')!.dataset['status']).toBe('failed');
    front.cells.scenario.set(null);
    expect(part.querySelectorAll('li')).toHaveLength(0);
    expect(part.dataset['status']).toBeUndefined();
  });
});
