import { fakeFront } from './fake-front';
import type { FakeFront } from './fake-front';
import { pageIntent } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

async function mount<
  K extends 'vsil-scenario-select' | 'vsil-case-select' | 'vsil-scenario-result',
>(tag: K, front: FakeFront): Promise<HTMLElementTagNameMap[K]> {
  const part = document.createElement(tag);
  part.client = front;
  document.body.append(part);
  await settle();
  return part;
}

afterEach(() => {
  document.body.replaceChildren();
  pageIntent.update({ scenario: '', case: '', variant: '' });
});

describe('<vsil-scenario-select>', () => {
  it('lists the scenarios of the status file, writes to page intent, and clears case on change', async () => {
    const front = fakeFront();
    const part = await mount('vsil-scenario-select', front);
    expect(front.spies.scenarios).toHaveBeenCalledWith({ file: 'simulation.yaml' });
    const select = part.querySelector('select')!;
    expect([...select.options].map((o) => o.textContent)).toEqual(['(no scenario)', 'a.yaml']);
    pageIntent.update({ case: 'old_case' });
    select.value = 'scenarios/a.yaml';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().scenario).toBe('scenarios/a.yaml');
    expect(pageIntent.get().case).toBeUndefined();
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

describe('<vsil-case-select>', () => {
  it('stays disabled with data-cases="none" until a multi-case scenario is chosen', async () => {
    const front = fakeFront();
    const part = await mount('vsil-case-select', front);
    const select = part.querySelector('select')!;
    expect(part.dataset['cases']).toBe('none');
    expect(select.disabled).toBe(true);
    expect(front.spies.check).not.toHaveBeenCalled();

    front.spies.check.mockResolvedValueOnce({
      result: 'usage',
      findings: [],
      scenario: {
        file: 'scenarios/cases.yaml',
        name: 'cases',
        case: null,
        cases: ['idle', 'full_load'],
        case_details: [
          { name: 'idle', only: null },
          { name: 'full_load', only: ['hil'] },
        ],
        steps: 0,
      },
    });
    pageIntent.update({ scenario: 'scenarios/cases.yaml', variant: 'sil' });
    await settle();

    expect(front.spies.check).toHaveBeenCalledWith(
      { scenario: 'scenarios/cases.yaml', variant: 'sil' },
      expect.objectContaining({ file: 'simulation.yaml', signal: expect.any(AbortSignal) }),
    );
    expect(part.dataset['cases']).toBe('available');
    expect(select.disabled).toBe(false);
    expect([...select.options].map((o) => [o.value, o.disabled])).toEqual([
      ['', false],
      ['idle', false],
      ['full_load', true],
    ]);

    select.value = 'idle';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().case).toBe('idle');

    // Switching to an incompatible variant clears the selected case when disabled
    front.spies.check.mockResolvedValueOnce({
      result: 'usage',
      findings: [],
      scenario: {
        file: 'scenarios/cases.yaml',
        name: 'cases',
        case: null,
        cases: ['idle', 'full_load'],
        case_details: [
          { name: 'idle', only: ['sil'] },
          { name: 'full_load', only: ['hil'] },
        ],
        steps: 0,
      },
    });
    pageIntent.update({ variant: 'hil' });
    await settle();
    expect(pageIntent.get().case).toBeUndefined();
    expect([...select.options].map((o) => [o.value, o.disabled])).toEqual([
      ['', false],
      ['idle', true],
      ['full_load', false],
    ]);
  });

  it('aborts an in-flight check when scenario changes or element disconnects', async () => {
    const front = fakeFront();
    const signals: AbortSignal[] = [];
    front.spies.check.mockImplementation((_intent: unknown, opts?: { signal?: AbortSignal }) => {
      if (opts?.signal) signals.push(opts.signal);
      return new Promise(() => undefined);
    });
    const part = await mount('vsil-case-select', front);
    pageIntent.update({ scenario: 'scenarios/a.yaml' });
    await settle();
    expect(signals).toHaveLength(1);
    expect(signals[0].aborted).toBe(false);

    pageIntent.update({ scenario: 'scenarios/b.yaml' });
    await settle();
    expect(signals).toHaveLength(2);
    expect(signals[0].aborted).toBe(true);
    expect(signals[1].aborted).toBe(false);

    part.remove();
    expect(signals[1].aborted).toBe(true);
  });

  it('resets stale options while check is in-flight, preserves pre-selected case until settled, and recovers after check failure', async () => {
    const front = fakeFront();
    front.spies.check.mockResolvedValueOnce({
      result: 'usage',
      findings: [],
      scenario: {
        file: 'scenarios/a.yaml',
        name: 'a',
        case: null,
        cases: ['caseA1', 'caseA2'],
        steps: 0,
      },
    });
    const part = await mount('vsil-case-select', front);
    const select = part.querySelector('select')!;

    // Load scenario A cases
    pageIntent.update({ scenario: 'scenarios/a.yaml' });
    await settle();
    expect(select.disabled).toBe(false);
    expect([...select.options].map((o) => o.value)).toEqual(['', 'caseA1', 'caseA2']);

    // Switch to scenario B with programmatic pre-selected case while check is in-flight
    let resolveB!: (v: unknown) => void;
    front.spies.check.mockImplementationOnce(
      () =>
        new Promise((resolve) => {
          resolveB = resolve;
        }),
    );
    pageIntent.update({ scenario: 'scenarios/b.yaml', case: 'caseB2' });
    // Immediately reset DOM so A's options cannot be selected while B's check is pending
    expect(select.disabled).toBe(true);
    expect(part.dataset['cases']).toBe('none');
    expect([...select.options].map((o) => o.value)).toEqual(['']);
    // Pre-selected case stays in pageIntent until B's check settles
    expect(pageIntent.get().case).toBe('caseB2');

    resolveB({
      result: 'usage',
      findings: [],
      scenario: {
        file: 'scenarios/b.yaml',
        name: 'b',
        case: null,
        cases: ['caseB1', 'caseB2'],
        steps: 0,
      },
    });
    await settle();
    expect(select.disabled).toBe(false);
    expect(part.dataset['cases']).toBe('available');
    expect(select.value).toBe('caseB2');
    expect(pageIntent.get().case).toBe('caseB2');

    // Switch to scenario C whose check fails: clears options and stale case without retry loop, then recovers on status update
    front.spies.check.mockRejectedValueOnce(new Error('503 check-busy'));
    pageIntent.update({ scenario: 'scenarios/c.yaml', case: 'caseB2' });
    await settle();
    expect(select.disabled).toBe(true);
    expect(part.dataset['cases']).toBe('none');
    expect([...select.options].map((o) => o.value)).toEqual(['']);
    expect(pageIntent.get().case).toBeUndefined();
    const callsAfterFailure = front.spies.check.mock.calls.length;

    // Subsequent status update (e.g. reconnect or run end) retries and recovers
    front.spies.check.mockResolvedValueOnce({
      result: 'usage',
      findings: [],
      scenario: {
        file: 'scenarios/c.yaml',
        name: 'c',
        case: null,
        cases: ['caseC1'],
        steps: 0,
      },
    });
    front.cells.status.patch({ link: 'online' });
    await settle();
    expect(front.spies.check).toHaveBeenCalledTimes(callsAfterFailure + 1);
    expect(select.disabled).toBe(false);
    expect(part.dataset['cases']).toBe('available');
    expect([...select.options].map((o) => o.value)).toEqual(['', 'caseC1']);
  });
});

describe('<vsil-scenario-result>', () => {
  it('shows status, case, params, step provenance (file:line and call_stack), and clears without scenario', async () => {
    const front = fakeFront();
    const part = await mount('vsil-scenario-result', front);
    expect(part.querySelector('output')!.textContent).toBe('-');
    front.cells.scenario.set({
      file: 'scenarios/cases.yaml',
      name: 'cases',
      case: 'full_load',
      params: [
        { name: 'target_rpm', value: 3000, from: 'cli', type: 'integer' },
        { name: 'tag', value: '<script>x</script>', from: 'default', type: 'string' },
      ],
      status: 'failed',
      handed_over: false,
      over_at_ms: 20,
      timeline_url: null,
      steps: [
        {
          index: 1,
          file: 'scenarios/templates/check.yaml',
          line: 42,
          name: 'rated check',
          text: 'expect speed',
          status: 'failed',
          at_ms: 20,
          message: 'too slow',
          call_stack: [
            {
              file: 'scenarios/cases.yaml',
              line: 15,
              name: 'call template',
              params: { rpm: 3000 },
            },
          ],
        },
      ],
    } as never);
    expect(part.dataset['status']).toBe('failed');
    expect(part.dataset['case']).toBe('full_load');
    expect(part.querySelector('output')!.textContent).toBe('failed');
    expect(part.querySelector('span[data-part="case"]')!.textContent).toBe('case: full_load');
    expect(part.querySelector('span[data-part="params"]')!.textContent).toBe(
      'params: target_rpm=3000, tag=<script>x</script>',
    );
    expect(part.querySelector('script')).toBeNull();
    const item = part.querySelector<HTMLLIElement>('ol > li')!;
    expect(item.dataset['status']).toBe('failed');
    expect(item.firstChild?.textContent).toBe(
      'rated check: expect speed — failed at 20 ms: too slow',
    );
    expect(item.querySelector('small[data-part="source"]')!.textContent).toBe(
      ' (scenarios/templates/check.yaml:42)',
    );
    expect(item.querySelector('ul[data-part="call-stack"] > li')!.textContent).toBe(
      'scenarios/cases.yaml:15 (call template) [rpm=3000]',
    );

    front.cells.scenario.set(null);
    expect(part.querySelectorAll('li')).toHaveLength(0);
    expect(part.dataset['status']).toBeUndefined();
    expect(part.dataset['case']).toBeUndefined();
    expect(part.querySelector<HTMLDivElement>('.vsil-scenario-meta')!.hidden).toBe(true);
  });
});

describe('<vsil-timeline>', () => {
  const run = (timeline_url: string | null) => ({
    file: 'scenarios/a.yaml',
    name: 'a',
    status: 'passed' as const,
    handed_over: false,
    over_at_ms: null,
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
