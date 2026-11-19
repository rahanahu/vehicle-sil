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
    expect(front.spies.scenarios).toHaveBeenCalledWith({ file: 'simulation.yaml' });
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
    front.spies.scenarios.mockResolvedValue([]);
    front.cells.status.patch({ file: 'b.yaml' });
    await settle();
    expect(front.spies.scenarios).toHaveBeenLastCalledWith({ file: 'b.yaml' });
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

describe('<vsil-timeline>', () => {
  const run = (timeline_url: string | null) => ({
    file: 'scenarios/a.yaml',
    name: 'a',
    status: 'passed' as const,
    steps: [],
    timeline_url,
  });

  async function mountTimeline(front: FakeFront, inner = '') {
    const part = document.createElement('vsil-timeline');
    part.innerHTML = inner;
    part.client = front;
    document.body.append(part);
    await settle();
    return part;
  }

  it("uses the page's own element, and changes only src and hidden", async () => {
    const front = fakeFront();
    const part = await mountTimeline(
      front,
      '<vehicle-sil-timeline compact hide="help tables"></vehicle-sil-timeline>',
    );
    const timeline = part.querySelector<HTMLElement>('vehicle-sil-timeline')!;
    expect(part.querySelectorAll('vehicle-sil-timeline')).toHaveLength(1);
    expect(timeline.hidden).toBe(true);
    expect(part.dataset['timeline']).toBe('none');
    front.cells.scenario.set(run('timelines/run.json'));
    expect(timeline.hidden).toBe(false);
    expect(timeline.getAttribute('src')).toBe('/timelines/run.json');
    expect(timeline.hasAttribute('compact')).toBe(true);
    expect(timeline.getAttribute('hide')).toBe('help tables');
    expect([...timeline.attributes].map((a) => a.name).sort()).toEqual(['compact', 'hide', 'src']);
    expect(part.dataset['timeline']).toBe('shown');
    front.cells.scenario.set(null);
    expect(timeline.hidden).toBe(true);
    expect(part.dataset['timeline']).toBe('none');
  });

  it('adds a default element when there is none, and removes only that one when it leaves', async () => {
    const front = fakeFront();
    const part = await mountTimeline(front);
    const made = part.querySelector<HTMLElement>('vehicle-sil-timeline')!;
    expect(made).not.toBeNull();
    front.cells.scenario.set(run('t.json'));
    expect(made.getAttribute('src')).toBe('/t.json');
    part.remove();
    expect(made.isConnected).toBe(false);
    expect(part.dataset['timeline']).toBeUndefined();

    const page = await mountTimeline(fakeFront(), '<vehicle-sil-timeline></vehicle-sil-timeline>');
    const own = page.querySelector('vehicle-sil-timeline')!;
    page.remove();
    expect(own.parentElement).toBe(page);
  });

  it('does not set the same file again', async () => {
    const front = fakeFront();
    const part = await mountTimeline(front);
    const timeline = part.querySelector<HTMLElement>('vehicle-sil-timeline')!;
    const set = vi.spyOn(timeline, 'setAttribute');
    front.cells.scenario.set(run('t.json'));
    front.cells.scenario.set({ ...run('t.json'), status: 'failed' });
    expect(set).toHaveBeenCalledTimes(1);
    front.cells.scenario.set(run('u.json'));
    expect(set).toHaveBeenCalledTimes(2);
  });

  it('works while <vehicle-sil-timeline> is not defined', async () => {
    expect(customElements.get('vehicle-sil-timeline')).toBeUndefined();
    const front = fakeFront();
    front.cells.scenario.set(run('t.json'));
    const part = await mountTimeline(front);
    expect(part.querySelector<HTMLElement>('vehicle-sil-timeline')!.getAttribute('src')).toBe(
      '/t.json',
    );
    expect(part.dataset['error']).toBeUndefined();
  });
});
