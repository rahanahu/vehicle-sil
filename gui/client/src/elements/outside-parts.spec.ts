import { ActionError } from '../index';
import type { CheckResult, ConnectEntry, Finding, Outside } from '../index';
import { fakeFront } from './fake-front';
import type { FakeFront } from './fake-front';
import { pageIntent } from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

async function mount<K extends 'vsil-connect' | 'vsil-outside'>(
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

const LIST: ConnectEntry[] = [
  {
    backend: 'packet',
    value: 'eth0',
    label: 'eth0',
    detail: 'up, connected',
    usable: true,
    reason: null,
  },
  {
    backend: 'packet',
    value: 'eth1',
    label: 'eth1',
    detail: 'up, in use by a vehicle-sil bridge (process 4321)',
    usable: true,
    reason: null,
  },
  {
    backend: 'tap',
    value: 'tap:lo',
    label: 'tap:lo',
    detail: '',
    usable: false,
    reason: 'not Ethernet',
  },
  { backend: 'npcap', error: 'vehicle_sil_ethernet_bridge is not next to vehicle_sil_launch' },
];

const finding = (fields: Partial<Finding>): Finding => ({
  severity: 'note',
  code: 'x',
  file: null,
  line: null,
  participant: null,
  network: null,
  message: 'm',
  detail: null,
  ...fields,
});

/** A check result as the parser makes it: the launcher's object in a `raw` that is not enumerable. */
const checked = (findings: Finding[], because: string[] = []): CheckResult => {
  const result = { findings } as unknown as CheckResult;
  Object.defineProperty(result, 'raw', { value: { time_lock: { locked: true, because } } });
  return result;
};

const options = (select: HTMLSelectElement) =>
  [...select.options].map((o) => [o.value, o.textContent, o.disabled]);

afterEach(() => {
  document.body.replaceChildren();
  pageIntent.update({ connect: undefined });
});

describe('<vsil-connect>', () => {
  it('offers SIL Kit only and asks for the list only when the user goes to the select', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    expect(options(select)).toEqual([['none', 'SIL Kit only (not connected)', false]]);
    expect(part.dataset['network']).toBe('ETH_EXT');
    expect(part.dataset['chosen']).toBe('none');
    expect(front.spies.connections).not.toHaveBeenCalled();
    expect(front.spies.check).not.toHaveBeenCalled();

    select.dispatchEvent(new Event('pointerdown'));
    select.dispatchEvent(new Event('focus')); // the same opening: one request
    expect(select.getAttribute('aria-busy')).toBe('true');
    await settle();
    expect(front.spies.connections).toHaveBeenCalledTimes(1);
    expect(front.spies.connections).toHaveBeenCalledWith(
      { network: 'ETH_EXT' },
      { file: 'simulation.yaml' },
    );
    expect(select.hasAttribute('aria-busy')).toBe(false);
    expect(options(select)).toEqual([
      ['none', 'SIL Kit only (not connected)', false],
      ['eth0', 'eth0 — up, connected', false],
      ['eth1', 'eth1 — up, in use by a vehicle-sil bridge (process 4321)', false],
      ['tap:lo', 'tap:lo — not Ethernet', true],
    ]);
    expect([...select.querySelectorAll('optgroup')].map((g) => g.label)).toEqual(['packet', 'tap']);
    // The backend that has no list says so, next to the select
    expect(part.querySelector('.vsil-why')!.textContent).toBe(
      'npcap: vehicle_sil_ethernet_bridge is not next to vehicle_sil_launch',
    );
    expect(select.getAttribute('aria-describedby')).toContain(part.querySelector('.vsil-why')!.id);

    // Going to it again asks again
    select.dispatchEvent(new Event('focus'));
    await settle();
    expect(front.spies.connections).toHaveBeenCalledTimes(2);
  });

  it('writes the choice into the page intent, checks it and shows the findings about the network as written', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    front.spies.check.mockResolvedValue(
      checked(
        [
          finding({
            code: 'connect-target-shared',
            network: 'ETH_EXT',
            severity: 'warning',
            message: 'shared.',
          }),
          finding({ code: 'outside-speed-set', message: 'real time.' }),
          finding({ code: 'unused-network', network: 'ETH_EXT', message: 'not about connecting.' }),
          finding({ code: 'connect-maybe-loop', network: 'CAN1', message: 'another network.' }),
        ],
        ['ETH_EXT'],
      ),
    );
    pageIntent.update({ scenario: 's.yaml', connect: { CAN1: 'can0' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    select.dispatchEvent(new Event('focus'));
    await settle();
    select.value = 'eth0';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0', ETH_EXT: 'eth0' });
    expect(part.dataset['chosen']).toBe('eth0');
    expect(front.spies.check).toHaveBeenCalledWith(
      { scenario: 's.yaml', connect: { CAN1: 'can0', ETH_EXT: 'eth0' } },
      { file: 'simulation.yaml' },
    );
    await settle();
    const shown = [...part.querySelectorAll('.vsil-findings li')] as HTMLElement[];
    expect(shown.map((li) => li.textContent)).toEqual(['shared.', 'real time.']);
    expect(shown.map((li) => li.dataset['code'])).toEqual([
      'connect-target-shared',
      'outside-speed-set',
    ]);
    expect(shown[0]!.dataset['severity']).toBe('warning');

    // SIL Kit only takes the network out of connect, and its findings go
    select.value = 'none';
    select.dispatchEvent(new Event('change'));
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0' });
    expect(part.querySelectorAll('.vsil-findings li')).toHaveLength(0);
    expect(front.spies.check).toHaveBeenCalledTimes(1);
    pageIntent.update({ connect: { ETH_EXT: 'eth1' } });
    expect(select.value).toBe('eth1');
    pageIntent.update({ connect: undefined });
    expect(select.value).toBe('none');
  });

  it('leaves out the run-wide outside findings when this network does not lock the time', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    front.spies.check.mockResolvedValue(
      checked([finding({ code: 'outside-speed-set', message: 'real time.' })], ['OTHER']),
    );
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    select.dispatchEvent(new Event('focus'));
    await settle();
    select.value = 'eth0';
    select.dispatchEvent(new Event('change'));
    await settle();
    expect(part.querySelectorAll('.vsil-findings li')).toHaveLength(0);
  });

  it('shows a refused check as written and lists again after target-not-listed', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    front.spies.check.mockRejectedValue(
      new ActionError(
        'target-not-listed',
        'there is no list of ETH_EXT: list it again.',
        'refused',
      ),
    );
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    select.dispatchEvent(new Event('focus'));
    await settle();
    // The new list no longer has the choice: it goes back to SIL Kit only
    front.spies.connections.mockResolvedValue(LIST.slice(1));
    select.value = 'eth0';
    select.dispatchEvent(new Event('change'));
    await settle();
    await settle();
    expect(part.querySelector('.vsil-findings')!.textContent).toBe(
      'there is no list of ETH_EXT: list it again.',
    );
    expect(front.spies.connections).toHaveBeenCalledTimes(2);
    expect(pageIntent.get().connect).toBeUndefined();
    expect(select.value).toBe('none');
  });

  it('shows a failed list as written, with its code in data-error, and keeps SIL Kit only', async () => {
    const front = fakeFront();
    front.spies.connections.mockRejectedValue(
      new ActionError('check-busy', 'a check is running: try again in a moment.', 'refused'),
    );
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    select.dispatchEvent(new Event('focus'));
    await settle();
    expect(part.dataset['error']).toBe('check-busy');
    expect(part.querySelector('.vsil-why')!.textContent).toBe(
      'a check is running: try again in a moment.',
    );
    expect(options(select)).toEqual([['none', 'SIL Kit only (not connected)', false]]);
    front.spies.connections.mockResolvedValue(LIST);
    select.dispatchEvent(new Event('focus'));
    await settle();
    expect(part.dataset['error']).toBeUndefined();
  });

  it('shows a choice that the page made itself, and drops it when the list cannot take it', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    pageIntent.update({ connect: { ETH_EXT: 'tap:lo' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    expect(select.value).toBe('tap:lo');
    expect(part.dataset['chosen']).toBe('tap:lo');
    // The list has it, but says it cannot be used: the run is not connected to it
    select.dispatchEvent(new Event('focus'));
    await settle();
    expect(pageIntent.get().connect).toBeUndefined();
    expect(select.value).toBe('none');
    expect(part.dataset['chosen']).toBe('none');
  });

  it('needs the network attribute, and cleans up when it leaves', async () => {
    const front = fakeFront();
    const part = await mount('vsil-connect', front);
    expect(part.querySelector('[role=alert]')!.textContent).toContain('network="<name>"');
    part.setAttribute('network', 'ETH_EXT');
    await settle();
    expect(part.querySelector('[role=alert]')).toBeNull();
    part.remove();
    expect(front.subscriptions()).toBe(0);
    expect(part.dataset['network']).toBeUndefined();
  });
});

const outside = (fields: Partial<Outside> = {}): Outside => ({
  network: 'ETH_EXT',
  target: 'eth0',
  participant: 'bridge.ETH_EXT',
  link: 'up',
  bus: null,
  condition: 'ok',
  reasons: [],
  nothing_received: false,
  to_outside: null,
  from_outside: null,
  ...fields,
});

describe('<vsil-outside>', () => {
  it('draws nothing without rows, and one row for each connected network', async () => {
    const front = fakeFront();
    const part = await mount('vsil-outside', front);
    const root = part.querySelector<HTMLElement>('.vsil')!;
    expect(root.hidden).toBe(true);
    front.cells.outside.set([
      outside({ to_outside: 12, from_outside: 3 }),
      outside({
        network: 'CAN1',
        target: 'can0',
        link: null,
        bus: 'bus-off',
        condition: 'interrupted',
        reasons: ['bus-off', 'frames-dropped'],
        nothing_received: true,
      }),
    ]);
    expect(root.hidden).toBe(false);
    const rows = [...part.querySelectorAll('li')] as HTMLElement[];
    expect(rows.map((r) => r.textContent)).toEqual([
      'ETH_EXT → eth0: link up, ok, to outside 12, from outside 3',
      'CAN1 → can0: bus bus-off, interrupted (bus-off, frames-dropped), to outside -, from outside -, nothing received',
    ]);
    expect(
      rows.map((r) => [r.dataset['network'], r.dataset['condition'], r.dataset['nothingReceived']]),
    ).toEqual([
      ['ETH_EXT', 'ok', 'false'],
      ['CAN1', 'interrupted', 'true'],
    ]);
    // The counts change every second: not a live region
    expect(part.querySelector('[aria-live]')).toBeNull();
    front.cells.outside.set([]);
    expect(root.hidden).toBe(true);
    part.remove();
    expect(front.subscriptions()).toBe(0);
  });
});
