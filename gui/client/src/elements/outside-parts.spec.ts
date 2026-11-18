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

/** Longer than the part waits for the page intent to settle before it checks. */
const settleCheck = () => new Promise((resolve) => setTimeout(resolve, 300));

/** A promise and the functions that end it. */
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

const open = async (select: HTMLSelectElement) => {
  select.dispatchEvent(new Event('pointerdown'));
  await settle();
};
const choose = (select: HTMLSelectElement, value: string) => {
  select.value = value;
  select.dispatchEvent(new Event('change'));
};
const lines = (part: HTMLElement, list: string) =>
  [...part.querySelectorAll(`.${list} li`)].map((li) => li.textContent);

afterEach(() => {
  document.body.replaceChildren();
  pageIntent.update({ connect: undefined, time_policy: undefined, scenario: '' });
});

describe('<vsil-connect>', () => {
  it('offers SIL Kit only and asks for the list only when the user opens the select', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    expect(options(select)).toEqual([['none', 'SIL Kit only (not connected)', false]]);
    expect(part.dataset['network']).toBe('ETH_EXT');
    expect(part.dataset['chosen']).toBe('none');
    // Passing by with Tab (and Shift, Escape) starts no bridge
    select.dispatchEvent(new Event('focus'));
    for (const key of ['Tab', 'Shift', 'Escape']) {
      select.dispatchEvent(new KeyboardEvent('keydown', { key }));
    }
    await settleCheck();
    expect(front.spies.connections).not.toHaveBeenCalled();
    expect(front.spies.check).not.toHaveBeenCalled();

    select.dispatchEvent(new Event('pointerdown'));
    select.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' })); // the same opening: one request
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
    expect(lines(part, 'vsil-why')).toEqual([
      'npcap: vehicle_sil_ethernet_bridge is not next to vehicle_sil_launch',
    ]);
    expect(select.getAttribute('aria-describedby')).toContain(part.querySelector('.vsil-why')!.id);

    // A key that opens it asks again
    select.dispatchEvent(new KeyboardEvent('keydown', { key: ' ' }));
    await settle();
    expect(front.spies.connections).toHaveBeenCalledTimes(2);
  });

  it('keeps the detail of a target that cannot be used, before the reason', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue([
      {
        backend: 'tap',
        value: 'tap:lo',
        label: 'lo',
        detail: 'down',
        usable: false,
        reason: 'not Ethernet',
      },
    ]);
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    await open(part.querySelector('select')!);
    expect(options(part.querySelector('select')!)[1]).toEqual([
      'tap:lo',
      'lo — down; not Ethernet',
      true,
    ]);
  });

  it('writes the choice into the page intent, checks it once it settles and shows the findings as written', async () => {
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
          finding({
            code: 'connect-shared-loop',
            network: 'CAN1',
            severity: 'error',
            message: 'a loop.',
          }),
          finding({
            code: 'outside-link-down',
            network: 'CAN1',
            message: 'not a connect finding.',
          }),
          finding({
            code: 'connect-tool-missing',
            network: 'CAN9',
            message: 'not connected here.',
          }),
        ],
        ['ETH_EXT'],
      ),
    );
    pageIntent.update({ scenario: 's.yaml', connect: { CAN1: 'can0', CAN9: 'none' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    await open(select);
    choose(select, 'eth1'); // moving through the choices with the arrows checks the last one only
    choose(select, 'eth0');
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0', CAN9: 'none', ETH_EXT: 'eth0' });
    expect(part.dataset['chosen']).toBe('eth0');
    expect(front.spies.check).not.toHaveBeenCalled();
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(1);
    expect(front.spies.check).toHaveBeenCalledWith(
      { scenario: 's.yaml', connect: { CAN1: 'can0', CAN9: 'none', ETH_EXT: 'eth0' } },
      { file: 'simulation.yaml' },
    );
    expect(lines(part, 'vsil-findings')).toEqual(['shared.', 'real time.', 'CAN1: a loop.']);
    const shown = [...part.querySelectorAll('.vsil-findings li')] as HTMLElement[];
    expect(shown.map((li) => li.dataset['code'])).toEqual([
      'connect-target-shared',
      'outside-speed-set',
      'connect-shared-loop',
    ]);
    expect(shown[0]!.dataset['severity']).toBe('warning');

    // Another part of the page changes the intent: checked again, and the findings follow
    front.spies.check.mockResolvedValue(
      checked([finding({ code: 'outside-not-real-time', message: 'free.' })], ['ETH_EXT']),
    );
    pageIntent.update({ time_policy: 'free' });
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(2);
    expect(lines(part, 'vsil-findings')).toEqual(['free.']);
    pageIntent.update({ time_policy: 'free' }); // the same intent again: nothing new to check
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(2);

    // SIL Kit only takes the network out of connect, and its findings go
    choose(select, 'none');
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0', CAN9: 'none' });
    expect(lines(part, 'vsil-findings')).toEqual([]);
    pageIntent.update({ scenario: '' });
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(2);
  });

  it('leaves out the run-wide outside findings when this network does not lock the time', async () => {
    const front = fakeFront();
    front.spies.check.mockResolvedValue(
      checked([finding({ code: 'outside-speed-set', message: 'real time.' })], ['OTHER']),
    );
    pageIntent.update({ connect: { ETH_EXT: 'eth0' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(1);
    expect(lines(part, 'vsil-findings')).toEqual([]);
  });

  it('checks a choice that the page made itself, and the parts of a page check the same intent', async () => {
    const front = fakeFront();
    // A check takes a while (the launcher describes every participant): the second part asks while it goes. The
    // parts ask for the same intent and file, which `check()` sends once (front.spec)
    const answer = deferred<CheckResult>();
    front.spies.check.mockReturnValue(answer.promise);
    pageIntent.update({ connect: { CAN1: 'can0', CAN2: 'can0' } });
    const first = await mount('vsil-connect', front, { network: 'CAN1' });
    const second = await mount('vsil-connect', front, { network: 'CAN2' });
    expect(first.querySelector('select')!.value).toBe('can0');
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledTimes(2);
    expect(front.spies.check.mock.calls[0]).toEqual(front.spies.check.mock.calls[1]);
    expect(front.spies.connections).not.toHaveBeenCalled();
    answer.resolve(
      checked([
        finding({
          code: 'connect-target-shared',
          network: 'CAN1',
          severity: 'warning',
          message: 'shared.',
        }),
      ]),
    );
    await settle();
    // The finding names the first network only: both parts show it
    expect(lines(first, 'vsil-findings')).toEqual(['shared.']);
    expect(lines(second, 'vsil-findings')).toEqual(['CAN1: shared.']);
  });

  it('drops the answer of a check that a newer choice has overtaken', async () => {
    const front = fakeFront();
    const late = deferred<CheckResult>();
    front.spies.check
      .mockReturnValueOnce(late.promise)
      .mockResolvedValue(
        checked([finding({ code: 'connect-x', network: 'ETH_EXT', message: 'about eth1.' })]),
      );
    front.spies.connections.mockResolvedValue(LIST);
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    await open(select);
    choose(select, 'eth0');
    await settleCheck();
    choose(select, 'eth1');
    await settleCheck();
    late.resolve(
      checked([finding({ code: 'connect-x', network: 'ETH_EXT', message: 'about eth0.' })]),
    );
    await settle();
    expect(lines(part, 'vsil-findings')).toEqual(['about eth1.']);
  });

  it('after target-not-listed lists again once, and checks again a choice that is still listed', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    const refusal = new ActionError(
      'target-not-listed',
      'there is no list of ETH_EXT: list it again.',
      'refused',
    );
    front.spies.check.mockRejectedValueOnce(refusal).mockResolvedValue(checked([]));
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    await open(select);
    choose(select, 'eth0');
    await settleCheck();
    await settle();
    expect(front.spies.connections).toHaveBeenCalledTimes(2);
    expect(front.spies.check).toHaveBeenCalledTimes(2);
    expect(lines(part, 'vsil-findings')).toEqual([]);
    expect(pageIntent.get().connect).toEqual({ ETH_EXT: 'eth0' });

    // Refused again after the new list: it stops there and shows the sentence
    front.spies.check.mockRejectedValue(refusal);
    choose(select, 'eth1');
    await settleCheck();
    await settle();
    expect(front.spies.connections).toHaveBeenCalledTimes(3);
    expect(front.spies.check).toHaveBeenCalledTimes(4);
    expect(lines(part, 'vsil-findings')).toEqual(['there is no list of ETH_EXT: list it again.']);
  });

  it('does not list after target-not-listed before the user has opened the choice', async () => {
    const front = fakeFront();
    front.spies.check.mockRejectedValue(
      new ActionError('target-not-listed', 'list it again.', 'refused'),
    );
    pageIntent.update({ connect: { ETH_EXT: 'eth0' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    await settleCheck();
    expect(lines(part, 'vsil-findings')).toEqual(['list it again.']);
    expect(front.spies.connections).not.toHaveBeenCalled();
  });

  it('says so when a new list takes the choice back to SIL Kit only, until the user chooses again', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    pageIntent.update({ connect: { ETH_EXT: 'packet:eth0' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    expect(select.value).toBe('packet:eth0');
    await open(select);
    expect(pageIntent.get().connect).toBeUndefined();
    expect(select.value).toBe('none');
    expect(part.dataset['reverted']).toBe('packet:eth0');
    expect(lines(part, 'vsil-why').at(-1)).toBe(
      "'packet:eth0' is not in the list of this computer: back to SIL Kit only",
    );
    expect(lines(part, 'vsil-findings')).toEqual([]);

    pageIntent.update({ connect: { ETH_EXT: 'tap:lo' } });
    await open(select);
    expect(part.dataset['reverted']).toBe('tap:lo');
    expect(lines(part, 'vsil-why').at(-1)).toBe(
      "'tap:lo' cannot be used (not Ethernet): back to SIL Kit only",
    );

    choose(select, 'eth0');
    expect(part.dataset['reverted']).toBeUndefined();
    expect(lines(part, 'vsil-why')).toEqual([
      'npcap: vehicle_sil_ethernet_bridge is not next to vehicle_sil_launch',
    ]);
  });

  it('shows a choice of the page as written, and takes that option away when the choice changes', async () => {
    const front = fakeFront();
    pageIntent.update({ connect: { ETH_EXT: 'eth9' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    expect(part.dataset['chosen']).toBe('eth9');
    pageIntent.update({ connect: { ETH_EXT: 'eth8' } });
    expect(options(select).map(([value]) => value)).toEqual(['none', 'eth8']);
    pageIntent.update({ connect: undefined });
    expect(options(select).map(([value]) => value)).toEqual(['none']);
  });

  it('shows a failed list as written, with its code in data-error, and keeps SIL Kit only', async () => {
    const front = fakeFront();
    front.spies.connections.mockRejectedValue(
      new ActionError('check-busy', 'a check is running: try again in a moment.', 'refused'),
    );
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT' });
    const select = part.querySelector('select')!;
    await open(select);
    expect(part.dataset['error']).toBe('check-busy');
    expect(lines(part, 'vsil-why')).toEqual(['a check is running: try again in a moment.']);
    expect(options(select)).toEqual([['none', 'SIL Kit only (not connected)', false]]);
    front.spies.connections.mockResolvedValue(LIST);
    await open(select);
    expect(part.dataset['error']).toBeUndefined();
  });

  it('takes its network out of the page intent when it leaves or its network changes', async () => {
    const front = fakeFront();
    pageIntent.update({ connect: { ETH_A: 'eth0', CAN1: 'can0' } });
    const part = await mount('vsil-connect', front, { network: 'ETH_A' });
    part.setAttribute('network', 'ETH_B');
    await settle();
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0' });
    pageIntent.update({ connect: { CAN1: 'can0', ETH_B: 'eth1' } });
    part.remove();
    expect(pageIntent.get().connect).toEqual({ CAN1: 'can0' });
    await settleCheck();
    expect(front.spies.check).not.toHaveBeenCalled(); // nothing is checked for a part that has left
  });

  it('lists and checks the file that its file attribute names, and drops its choice when the file changes', async () => {
    const front = fakeFront();
    front.spies.connections.mockResolvedValue(LIST);
    const part = await mount('vsil-connect', front, { network: 'ETH_EXT', file: 'connect.yaml' });
    const select = part.querySelector('select')!;
    select.dispatchEvent(new Event('pointerdown'));
    await settle();
    expect(front.spies.connections).toHaveBeenCalledWith(
      { network: 'ETH_EXT' },
      { file: 'connect.yaml' },
    );
    select.value = 'eth0';
    select.dispatchEvent(new Event('change'));
    await settleCheck();
    expect(front.spies.check).toHaveBeenCalledWith(
      { connect: { ETH_EXT: 'eth0' } },
      { file: 'connect.yaml' },
    );
    part.setAttribute('file', 'other.yaml');
    await settle();
    expect(pageIntent.get().connect).toBeUndefined();
    part.remove();
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
      outside({ network: 'ETH_2', target: 'eth2', link: null }),
    ]);
    expect(root.hidden).toBe(false);
    const rows = [...part.querySelectorAll('li')] as HTMLElement[];
    expect(rows.map((r) => r.textContent)).toEqual([
      'ETH_EXT → eth0: link up, ok, to outside 12, from outside 3',
      'CAN1 → can0: bus bus-off, interrupted (bus-off, frames-dropped), to outside -, from outside -, nothing received',
      'ETH_2 → eth2: -, ok, to outside -, from outside -',
    ]);
    expect(
      rows.map((r) => [r.dataset['network'], r.dataset['condition'], r.dataset['nothingReceived']]),
    ).toEqual([
      ['ETH_EXT', 'ok', 'false'],
      ['CAN1', 'interrupted', 'true'],
      ['ETH_2', 'ok', 'false'],
    ]);
    // The counts change every second: not a live region
    expect(part.querySelector('[aria-live]')).toBeNull();
    front.cells.outside.set([]);
    expect(root.hidden).toBe(true);
    part.remove();
    expect(front.subscriptions()).toBe(0);
  });
});
