import { fakeFront } from './fake-front';
import './index';
import { pageIntent, setReasonText } from './index';

// What `connect()` does in this test, and the arguments it was called with (a plain function: a spy would count a
// rejected promise as unhandled a second time).
const page = vi.hoisted(() => ({
  calls: [] as unknown[][],
  connect: (() => Promise.reject(new Error('no connection set'))) as () => Promise<unknown>,
}));

vi.mock('../index', async (original) => ({
  ...(await original<typeof import('../index')>()),
  connect: (...args: unknown[]) => {
    page.calls.push(args);
    return page.connect();
  },
}));

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

beforeEach(() => (page.calls.length = 0));
afterEach(() => {
  document.body.replaceChildren();
  setReasonText(null);
  pageIntent.update({ scenario: '' });
});

describe('the life of a part', () => {
  it('subscribes in the document and stops all of it when it is removed', async () => {
    const front = fakeFront();
    const part = document.createElement('vsil-time-controls');
    part.client = front;
    document.body.append(part);
    await settle();
    expect(front.subscriptions()).toBeGreaterThan(0);
    part.remove();
    expect(front.subscriptions()).toBe(0);
    expect(part.querySelector('.vsil')).toBeNull();
  });

  it('draws nothing when it leaves while it waits for the connection', async () => {
    const front = fakeFront();
    let arrive: (value: typeof front) => void = () => undefined;
    page.connect = () => new Promise((resolve) => (arrive = resolve as never));
    const part = document.createElement('vsil-status');
    document.body.append(part);
    await Promise.resolve(); // let the scheduled microtask start connect()
    expect(page.calls).toHaveLength(1);
    part.remove();
    arrive(front);
    await settle();
    expect(part.children.length).toBe(0);
    expect(front.subscriptions()).toBe(0);
  });

  it('cancels a scheduled bind immediately on disconnect before the microtask runs', async () => {
    const front = fakeFront();
    page.connect = async () => front;
    const part = document.createElement('vsil-status');
    document.body.append(part);
    part.remove();
    await settle();
    expect(page.calls).toHaveLength(0);
    expect(part.children.length).toBe(0);
    expect(front.subscriptions()).toBe(0);
  });

  it('coalesces multiple synchronous property and attribute updates into a single bind', async () => {
    const front = fakeFront();
    const variableSpy = vi.spyOn(front, 'variable');
    const part = document.createElement('vsil-value');
    document.body.append(part);
    part.client = front;
    part.variable = 'Engine.e.flag';
    part.on = 'ACTIVE';
    part.off = 'idle';
    part.noUnit = true;
    await settle();
    expect(variableSpy).toHaveBeenCalledTimes(1);
    expect(variableSpy).toHaveBeenCalledWith('Engine.e.flag');
    expect(part.querySelectorAll('.vsil')).toHaveLength(1);

    // Subsequent synchronous changes also coalesce into one re-bind.
    part.variable = 'Engine.e.speed';
    part.noUnit = false;
    part.on = 'YES';
    await settle();
    expect(variableSpy).toHaveBeenCalledTimes(2);
    expect(variableSpy).toHaveBeenLastCalledWith('Engine.e.speed');
    expect(front.variables.get('Engine.e.flag')!.cell.listeners.size).toBe(0);
    expect(front.variables.get('Engine.e.speed')!.cell.listeners.size).toBe(1);
    expect(part.querySelectorAll('.vsil')).toHaveLength(1);
  });

  it('binds once when disconnected and reconnected in the same synchronous turn', async () => {
    const front = fakeFront();
    const variableSpy = vi.spyOn(front, 'variable');
    const part = document.createElement('vsil-value');
    part.client = front;
    part.variable = 'Engine.e.speed';
    document.body.append(part);
    part.remove();
    document.body.append(part);
    await settle();
    expect(variableSpy).toHaveBeenCalledTimes(1);
    expect(part.querySelectorAll('.vsil')).toHaveLength(1);
    expect(front.variables.get('Engine.e.speed')!.cell.listeners.size).toBe(1);
  });

  it('uses the shared connection, called without options', async () => {
    const front = fakeFront();
    page.connect = async () => front;
    document.body.append(
      document.createElement('vsil-status'),
      document.createElement('vsil-run-controls'),
    );
    await settle();
    expect(page.calls).toEqual([[], []]);
  });

  it('binds again when an attribute changes, without leaving the old subscription', async () => {
    const front = fakeFront();
    const part = document.createElement('vsil-value');
    part.client = front;
    part.setAttribute('variable', 'A.a');
    document.body.append(part);
    await settle();
    part.setAttribute('variable', 'B.b');
    await settle();
    expect(front.variables.get('A.a')!.cell.listeners.size).toBe(0);
    expect(front.variables.get('B.b')!.cell.listeners.size).toBe(1);
    expect(part.querySelectorAll('output')).toHaveLength(1);
  });

  it('binds again when a client is set later, and does not double its content', async () => {
    const first = fakeFront();
    const second = fakeFront();
    const part = document.createElement('vsil-status');
    part.client = first;
    document.body.append(part);
    await settle();
    part.client = second;
    await settle();
    expect(first.subscriptions()).toBe(0);
    expect(second.subscriptions()).toBeGreaterThan(0);
    expect(part.querySelectorAll('.vsil')).toHaveLength(1);
  });

  it('does not bind again when the same client is set again', async () => {
    const front = fakeFront();
    const part = document.createElement('vsil-status');
    part.client = front;
    document.body.append(part);
    await settle();
    const root = part.querySelector('.vsil');
    part.client = front;
    await settle();
    expect(part.querySelector('.vsil')).toBe(root);
  });

  it('normalizes client = undefined to null without redundant re-binds', async () => {
    const front = fakeFront();
    page.connect = async () => front;
    const part = document.createElement('vsil-status');
    expect(part.client).toBeNull();
    (part as unknown as { client: unknown }).client = undefined;
    expect(part.client).toBeNull();

    document.body.append(part);
    await settle();
    const root = part.querySelector('.vsil');
    (part as unknown as { client: unknown }).client = undefined;
    await settle();
    expect(part.client).toBeNull();
    expect(part.querySelector('.vsil')).toBe(root);
    expect(page.calls).toHaveLength(1);

    part.client = front;
    await settle();
    expect(part.client).toBe(front);
    (part as unknown as { client: unknown }).client = undefined;
    expect(part.client).toBeNull();
  });

  it('keeps the children the page put in it', async () => {
    const front = fakeFront();
    const part = document.createElement('vsil-status');
    part.append(document.createElement('em'));
    part.client = front;
    document.body.append(part);
    await settle();
    part.remove();
    document.body.append(part);
    await settle();
    expect(part.querySelectorAll('em')).toHaveLength(1);
    expect(part.querySelectorAll('.vsil')).toHaveLength(1);
  });

  it('keeps a client that the page set before the element was defined', async () => {
    const front = fakeFront();
    // A document without the definitions: the element is upgraded when it enters this one.
    const part = document.implementation
      .createHTMLDocument()
      .createElement('vsil-time-controls') as HTMLElement & {
      client?: unknown;
      stepMs?: number | string;
      speeds?: string;
      labelPause?: string;
    };
    part.client = front;
    part.stepMs = 25;
    part.speeds = '1 2 max';
    part.labelPause = 'Hold';
    document.body.append(part);
    await settle();
    expect(front.subscriptions()).toBeGreaterThan(0);
    expect(part.getAttribute('step-ms')).toBe('25');
    expect(part.getAttribute('speeds')).toBe('1 2 max');
    expect(part.querySelector('input')!.value).toBe('25');
    expect(part.querySelector('button')!.textContent).toBe('Hold');
  });

  it('upgrades pre-definition falsy, boolean and string properties across attribute-bearing parts', async () => {
    const front = fakeFront();
    const inert = document.implementation.createHTMLDocument();

    const writeFalse = inert.createElement('vsil-write') as HTMLElement & {
      client?: unknown;
      variable?: string;
      value?: unknown;
      label?: string;
    };
    writeFalse.client = front;
    writeFalse.variable = 'E.e.connected';
    writeFalse.value = false;
    writeFalse.label = 'Disconnect';

    const writeZero = inert.createElement('vsil-write') as HTMLElement & {
      client?: unknown;
      variable?: string;
      value?: unknown;
    };
    writeZero.client = front;
    writeZero.variable = 'E.e.throttle';
    writeZero.value = 0;

    const valueEl = inert.createElement('vsil-value') as HTMLElement & {
      client?: unknown;
      variable?: string;
      on?: string;
      off?: string;
      noUnit?: boolean;
    };
    valueEl.client = front;
    valueEl.variable = 'E.e.speed';
    valueEl.on = 'HI';
    valueEl.off = 'lo';
    valueEl.noUnit = true;

    const connectEl = inert.createElement('vsil-connect') as HTMLElement & {
      client?: unknown;
      network?: string;
      file?: string;
    };
    connectEl.client = front;
    connectEl.network = 'CAN1';
    connectEl.file = 'bench.yaml';

    const runEl = inert.createElement('vsil-run-controls') as HTMLElement & {
      client?: unknown;
      labelStart?: string;
      labelStop?: string;
      labelForceStop?: string;
    };
    runEl.client = front;
    runEl.labelStart = 'Go';
    runEl.labelStop = 'Halt';
    runEl.labelForceStop = 'Kill';

    document.body.append(writeFalse, writeZero, valueEl, connectEl, runEl);
    await settle();

    expect(writeFalse.getAttribute('value')).toBe('false');
    expect(writeFalse.querySelector('button')!.textContent).toBe('Disconnect');
    writeFalse.querySelector('button')!.click();
    expect(front.variables.get('E.e.connected')!.write).toHaveBeenCalledWith(false);

    expect(writeZero.getAttribute('value')).toBe('0');
    writeZero.querySelector('button')!.click();
    expect(front.variables.get('E.e.throttle')!.write).toHaveBeenCalledWith(0);

    expect(valueEl.hasAttribute('no-unit')).toBe(true);
    expect(valueEl.getAttribute('on')).toBe('HI');
    expect(valueEl.getAttribute('off')).toBe('lo');

    expect(connectEl.getAttribute('network')).toBe('CAN1');
    expect(connectEl.getAttribute('file')).toBe('bench.yaml');
    expect(connectEl.dataset['network']).toBe('CAN1');

    expect([...runEl.querySelectorAll('button')].map((b) => b.textContent)).toEqual([
      'Go',
      'Halt',
      'Kill',
    ]);
  });

  it('says what went wrong when it cannot connect', async () => {
    page.connect = () => Promise.reject(new Error('open the URL the server printed'));
    const part = document.createElement('vsil-status');
    document.body.append(part);
    await settle();
    expect(part.querySelector('[role=alert]')?.textContent).toBe('open the URL the server printed');
  });

  describe('when the shared connection fails', () => {
    const alerts = () => document.querySelectorAll('[role=alert]');
    const make = (...tags: string[]) => {
      const parts = tags.map(
        (tag) => document.createElement(tag) as HTMLElement & { client?: unknown },
      );
      document.body.append(...parts);
      return parts;
    };
    beforeEach(() => {
      page.connect = () => Promise.reject(new Error('open the URL the server printed'));
    });

    it('tells it once, by the status, and marks every part', async () => {
      const parts = make(
        'vsil-run-controls',
        'vsil-time-controls',
        'vsil-status',
        'vsil-run-controls',
      );
      await settle();
      expect(alerts()).toHaveLength(1);
      expect(parts[2].querySelector('[role=alert]')?.textContent).toBe(
        'open the URL the server printed',
      );
      for (const part of parts) expect(part.getAttribute('data-error')).toBe('error');
    });

    it('tells it by the first part when there is no status', async () => {
      const parts = make('vsil-run-controls', 'vsil-time-controls');
      await settle();
      expect(alerts()).toHaveLength(1);
      expect(parts[0].querySelector('[role=alert]')).not.toBeNull();
      expect(parts[1].getAttribute('data-error')).toBe('error');
    });

    it('passes the sentence on when the part that tells it leaves', async () => {
      const parts = make('vsil-status', 'vsil-run-controls', 'vsil-time-controls');
      await settle();
      parts[0].remove();
      await settle();
      expect(alerts()).toHaveLength(1);
      expect(parts[1].querySelector('[role=alert]')).not.toBeNull();
      expect(parts[0].getAttribute('data-error')).toBeNull();
    });

    it('moves the sentence to a status that is added later, and never shows two', async () => {
      const parts = make('vsil-run-controls');
      await settle();
      expect(parts[0].querySelector('[role=alert]')).not.toBeNull();
      const [status] = make('vsil-status');
      await settle();
      expect(alerts()).toHaveLength(1);
      expect(status.querySelector('[role=alert]')).not.toBeNull();
      expect(parts[0].getAttribute('data-error')).toBe('error');
    });

    it('removes the mark and the sentence when the part binds again with a connection', async () => {
      const parts = make('vsil-status', 'vsil-time-controls');
      await settle();
      const front = fakeFront();
      parts[0].client = front;
      parts[1].client = front;
      await settle();
      expect(document.body.textContent).not.toContain('open the URL');
      for (const part of parts) expect(part.getAttribute('data-error')).toBeNull();
    });

    it('lets a part with its own connection tell its own failure', async () => {
      const own = document.createElement('vsil-time-controls');
      own.client = new Proxy(fakeFront(), {
        get: () => {
          throw new Error('own connection failed');
        },
      });
      const [status] = make('vsil-status');
      document.body.append(own);
      await settle();
      expect(alerts()).toHaveLength(2);
      expect(own.querySelector('[role=alert]')?.textContent).toBe('own connection failed');
      expect(status.querySelector('[role=alert]')?.textContent).toBe(
        'open the URL the server printed',
      );
    });

    it('lets a part tell what is wrong with it alone', async () => {
      page.connect = async () => fakeFront();
      const [status, value] = make('vsil-status', 'vsil-value');
      await settle();
      expect(value.querySelector('[role=alert]')?.textContent).toContain('variable=');
      expect(value.getAttribute('data-error')).toBe('error');
      expect(status.getAttribute('data-error')).toBeNull();
    });
  });

  it('can be loaded twice without failing', async () => {
    vi.resetModules();
    // A second evaluation of the module is what a page that loads two copies does.
    // eslint-disable-next-line no-restricted-syntax
    await expect(import('./index')).resolves.toBeDefined();
  });
});
