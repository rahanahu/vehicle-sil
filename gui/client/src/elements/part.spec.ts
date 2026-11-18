// @vitest-environment jsdom
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
    part.remove();
    arrive(front);
    await settle();
    expect(part.children.length).toBe(0);
    expect(front.subscriptions()).toBe(0);
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
    };
    part.client = front;
    document.body.append(part);
    await settle();
    expect(front.subscriptions()).toBeGreaterThan(0);
  });

  it('says what went wrong when it cannot connect', async () => {
    page.connect = () => Promise.reject(new Error('open the URL the server printed'));
    const part = document.createElement('vsil-status');
    document.body.append(part);
    await settle();
    expect(part.querySelector('[role=alert]')?.textContent).toBe('open the URL the server printed');
  });

  it('can be loaded twice without failing', async () => {
    vi.resetModules();
    // A second evaluation of the module is what a page that loads two copies does.
    // eslint-disable-next-line no-restricted-syntax
    await expect(import('./index')).resolves.toBeDefined();
  });
});
