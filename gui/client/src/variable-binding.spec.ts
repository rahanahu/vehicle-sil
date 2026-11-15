import { GuiClient, createClient } from './client';
import { BATCH_MS } from './connection';
import { FakeHttp, FakeRequest } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { BoundVariable, DROPPED_MESSAGE, VariableBinding } from './variable-binding';
import { VariableRef } from './variables';

// client.variables.bind: the value state of one variable, whether it can be written and why not, watching only while
// visible, and binds of the same variable sharing the write status. The write status transitions themselves are tested
// in variable-writes.spec.ts.

const variable = (name: string, fields: object) => ({
  name,
  type: 'real',
  role: 'input',
  writable: true,
  unit: '',
  min: null,
  max: null,
  step: null,
  description: '',
  enum: null,
  ...fields,
});

const SCHEMA = [
  variable('engine.demand', { unit: '%', min: 0, max: 100 }),
  variable('engine.speed', { type: 'integer', role: 'output', writable: false, unit: 'rpm' }),
  variable('engine.enabled', { type: 'bool' }),
  variable('engine.mode', {
    type: 'enum',
    role: 'parameter',
    enum: [
      { name: 'off', value: 0 },
      { name: 'on', value: 1 },
    ],
  }),
  variable('engine.offset', { unit: 'rpm' }),
];

const variables = (state = 'ready', connected = true, more: object = {}) => ({
  state,
  reason: null,
  peers: [{ name: 'Engine', connected, variables: SCHEMA }],
  peers_omitted: 0,
  variables_omitted: 0,
  operations: [],
  ...more,
});

const hello = (state = 'ready', id = 1, session: object = {}) => ({
  type: 'hello',
  vehicle_sil_gui: 1,
  version: '0.1.0',
  instance: 'x',
  session: {
    id,
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: variables(state),
    ...session,
  },
  replay: null,
});

const values = (
  sampleMs: number,
  paused: boolean,
  peerValues: Record<string, unknown>,
  session = 1,
) => ({
  type: 'values',
  id: 1,
  session,
  sim_ms: sampleMs,
  paused,
  peers: { Engine: { sample_ms: sampleMs, values: peerValues } },
});

const changed = (state = 'ready', connected = true, more: object = {}) => ({
  type: 'variables',
  session: 1,
  variables: variables(state, connected, more),
});

const operation = (seq: number, state: string, done: boolean) => ({
  type: 'operation',
  session: 1,
  seq,
  state,
  done,
  peers: { Engine: { state, applied_at_ms: 200, late: false, reason: null } },
});

const ref = (name: string): VariableRef => ({ peer: 'Engine', name });

describe('client.variables.bind', () => {
  let fake: FakeWebSockets;
  let http: FakeHttp;
  let client: GuiClient;

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    http = new FakeHttp();
    client = createClient({
      baseUrl: 'http://127.0.0.1:8765/',
      token: 'secret',
      socket: fake.factory,
      http: http.transport,
      onError: (error) => {
        throw error;
      },
    });
    client.connect();
    socket().open();
  });

  afterEach(() => {
    client.close();
    vi.useRealTimers();
  });

  const socket = () => fake.sockets[fake.sockets.length - 1];

  async function deliver(...messages: unknown[]) {
    for (const message of messages) socket().send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
  }

  /** The watch and unwatch the client sent. */
  const watches = () =>
    socket().sent.filter((m) => (m as { type: string }).type !== 'auth') as {
      type: string;
      variables?: unknown;
      period_ms?: number;
    }[];

  /** Takes an api/write not answered yet. */
  const requests = (): FakeRequest[] => {
    const found = http.requests.filter((r) => r.request.path === 'api/write');
    for (const request of found) http.requests.splice(http.requests.indexOf(request), 1);
    return found;
  };

  it('tells a value that is waited for, live, of a stopped time, old, and the last of a run', async () => {
    const speed = client.variables.bind(ref('engine.speed'));
    const state = () => speed.state.get();
    expect(state().condition).toEqual({ kind: 'unavailable', status: 'unknown' });
    await deliver(hello());
    expect(state().condition).toEqual({ kind: 'waiting' });
    expect(state().info).toMatchObject({ name: 'engine.speed', unit: 'rpm' });
    expect(state().ref).toEqual(ref('engine.speed'));
    expect(watches()).toContainEqual({
      type: 'watch',
      id: 1,
      variables: [ref('engine.speed')],
      period_ms: 100,
    });
    await deliver(values(1200, false, { 'engine.speed': 1450 }));
    expect(state().reading).toEqual({ value: 1450, sampleMs: 1200, simMs: 1200, paused: false });
    expect(state().condition).toEqual({ kind: 'live', reading: state().reading });
    await deliver(values(1300, true, { 'engine.speed': 1460 }));
    expect(state().condition.kind).toBe('paused');
    // The ECU disconnected: the value is kept, but stale
    await deliver(changed('ready', false));
    expect(state().condition).toMatchObject({ kind: 'stale', why: 'peer' });
    // Reading values keeps failing (a reason while still ready)
    await deliver(changed('ready', true, { reason: 'the values could not be read' }));
    expect(state().condition).toMatchObject({ kind: 'stale', why: 'reading' });
    await deliver(changed('ended', false));
    expect(state().condition).toMatchObject({ kind: 'ended', reading: { value: 1460 } });
    // The connection between the client and the server dropped
    socket().fail();
    expect(state().condition).toMatchObject({ kind: 'stale', why: 'connection' });
    expect(state().block).toBe('read-only');
    speed.close();
  });

  it('says that a variable is not in this run', async () => {
    const nothing = client.variables.bind(ref('engine.nothing'));
    await deliver(hello());
    expect(nothing.state.get()).toMatchObject({
      info: undefined,
      condition: { kind: 'missing' },
      block: 'missing',
    });
    // A name that matches a key of the prototype is still a missing variable
    const odd = client.variables.bind({ peer: 'constructor', name: 'toString' });
    expect(odd.state.get().condition).toEqual({ kind: 'missing' });
    nothing.close();
    odd.close();
  });

  it('watches only while it is visible, and stops when it is closed', async () => {
    await deliver(hello());
    const speed = client.variables.bind(ref('engine.speed'), { visible: false });
    await deliver(values(100, false, { 'engine.speed': 800 }));
    expect(watches()).toEqual([]);
    expect(speed.state.get().reading).toBeUndefined();
    speed.setVisible(true);
    speed.setVisible(true); // No change if the same
    await deliver(values(200, false, { 'engine.speed': 900 }));
    expect(watches().map((m) => m.type)).toEqual(['watch']);
    expect(speed.state.get().reading).toMatchObject({ value: 900 });
    speed.setVisible(false);
    expect(speed.state.get().reading).toBeUndefined();
    expect(speed.state.get().condition).toEqual({ kind: 'waiting' });
    await deliver();
    expect(watches().map((m) => m.type)).toEqual(['watch', 'unwatch']);
    speed.setVisible(true);
    await deliver(values(300, false, { 'engine.speed': 1000 }));
    expect(speed.state.get().reading).toMatchObject({ value: 1000 });
    speed.close();
    speed.close();
    speed.setVisible(true); // Does nothing after close
    await deliver();
    expect(watches().map((m) => m.type)).toEqual(['watch', 'unwatch', 'watch', 'unwatch']);
  });

  it('keeps the history it was asked for', async () => {
    await deliver(hello());
    const graph = client.variables.bind(ref('engine.speed'), { history: 10, periodMs: 50 });
    const plain = client.variables.bind(ref('engine.speed'));
    await deliver();
    expect(watches()[0]).toMatchObject({ type: 'watch', period_ms: 50 });
    const seen: number[] = [];
    graph.state.subscribe(() => seen.push(graph.history().length));
    for (const [ms, rpm] of [
      [100, 800],
      [200, 900],
      [300, 1000],
    ]) {
      await deliver(values(ms, false, { 'engine.speed': rpm }));
    }
    expect(graph.history()).toEqual([
      { sampleMs: 100, value: 800 },
      { sampleMs: 200, value: 900 },
      { sampleMs: 300, value: 1000 },
    ]);
    // state notifies whenever a point is added (a graph reads history() again on that notification)
    expect(seen).toEqual([1, 2, 3]);
    expect(plain.history()).toEqual([]);
    graph.setVisible(false);
    expect(graph.history()).toEqual([]);
    graph.close();
    plain.close();
  });

  it('says why a variable cannot be written, and does not write then', async () => {
    const enabled = client.variables.bind(ref('engine.enabled'));
    const block = () => enabled.state.get().block;
    expect(block()).toBe('not-ready');
    await deliver(hello());
    expect(block()).toBeNull();
    await deliver(changed('ready', false));
    expect(block()).toBe('peer-disconnected');
    enabled.write(true);
    await deliver(changed('ended'));
    expect(block()).toBe('ended');
    enabled.write(true);
    await deliver(changed('connecting'));
    expect(block()).toBe('not-ready');
    await deliver(changed());
    expect(block()).toBeNull();
    socket().fail();
    expect(block()).toBe('disconnected');
    enabled.write(true);
    expect(requests()).toEqual([]);
    expect(enabled.state.get().writeStatus).toEqual({ kind: 'idle' });
    enabled.close();
  });

  it('lets a parameter be written only before the first step of a run that started paused', async () => {
    const mode = client.variables.bind(ref('engine.mode'));
    await deliver(hello());
    expect(mode.state.get().block).toBe('parameter');
    await deliver(hello('ready', 2, { start_paused: true, phase: 'waiting' }));
    expect(client.time.state.get().beforeStart).toBe(true);
    expect(mode.state.get().block).toBeNull();
    mode.write('on');
    expect(requests().map((r) => r.request.body)).toEqual([
      { session: 2, writes: { Engine: { 'engine.mode': 'on' } } },
    ]);
    mode.close();
  });

  it('only shows the variables while a scenario runs', async () => {
    const enabled = client.variables.bind(ref('engine.enabled'));
    const scenario = {
      file: 'scenarios/a.yaml',
      name: null,
      step_count: null,
      steps: [],
      steps_omitted: 0,
      status: null,
      over_at_ms: null,
    };
    await deliver(hello('ready', 1, { scenario }));
    expect(client.session.session.get()?.scenario).not.toBeNull();
    expect(enabled.state.get().block).toBe('scenario');
    enabled.write(true);
    expect(requests()).toEqual([]);
    enabled.close();
  });

  it('does not let a part that needs a value or a range write without it', async () => {
    await deliver(hello());
    const slider = client.variables.bind(ref('engine.demand'), {
      needsRange: true,
      needsValue: true,
    });
    expect(slider.state.get().block).toBe('no-value');
    slider.write(40);
    expect(requests()).toEqual([]);
    await deliver(values(100, false, { 'engine.demand': 20 }));
    expect(slider.state.get().block).toBeNull();
    // Without a received value, a value sent is enough to move it
    const other = client.variables.bind(ref('engine.demand'));
    other.write(30);
    slider.setVisible(false);
    expect(slider.state.get()).toMatchObject({ reading: undefined, pending: 30, block: null });
    const unbounded = client.variables.bind(ref('engine.offset'), { needsRange: true });
    expect(unbounded.state.get().block).toBe('no-range');
    // Widget-independent reasons come first
    await deliver(changed('ended'));
    expect(unbounded.state.get().block).toBe('ended');
    for (const bound of [slider, other, unbounded]) bound.close();
  });

  it('keeps one write state per variable: shared by two bindings and kept when one is made again', async () => {
    await deliver(hello());
    const first = client.variables.bind(ref('engine.enabled'));
    const second = client.variables.bind(ref('engine.enabled'));
    const seen: BoundVariable[] = [];
    second.state.subscribe((state) => seen.push(state));
    first.write(true);
    // The subscribers of the other bind are notified too, before anyone reads
    expect(seen.map((s) => s.writeStatus.kind)).toEqual(['sending']);
    expect(second.state.get()).toMatchObject({ writeStatus: { kind: 'sending' }, pending: true });
    // A write from the other bind is not sent on top (once received, the last value is sent)
    second.write(false);
    const [request] = requests();
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'engine.enabled': true } },
    });
    expect(first.state.get().pending).toBe(false);
    first.close();
    const again = client.variables.bind(ref('engine.enabled'), { visible: false });
    expect(again.state.get()).toMatchObject({ writeStatus: { kind: 'sending' }, pending: false });
    request.flush({ session_id: 1, operation: 4 });
    expect(requests().map((r) => r.request.body)).toEqual([
      { session: 1, writes: { Engine: { 'engine.enabled': false } } },
    ]);
    // A bind of another variable does not see this write
    const other = client.variables.bind(ref('engine.offset'), { visible: false });
    expect(other.state.get()).toMatchObject({ writeStatus: { kind: 'idle' }, pending: null });
    for (const bound of [second, again, other]) bound.close();
  });

  it('forgets the writes of a run when the next one starts', async () => {
    await deliver(hello());
    const enabled = client.variables.bind(ref('engine.enabled'));
    enabled.write(true);
    requests()[0].flush({ session_id: 1, operation: 4 });
    expect(enabled.state.get()).toMatchObject({
      writeStatus: { kind: 'queued', seq: 4 },
      pending: true,
    });
    await deliver(hello('ready', 2), values(0, false, { 'engine.enabled': false }, 2));
    expect(enabled.state.get()).toMatchObject({
      writeStatus: { kind: 'idle' },
      pending: null,
      reading: { value: false },
    });
    enabled.close();
  });

  it('does not send a value kept back once the variable cannot be written, and says so', async () => {
    await deliver(hello());
    const enabled = client.variables.bind(ref('engine.enabled'));
    enabled.write(true);
    enabled.write(false); // While waiting for the previous write: only held
    const [request] = requests();
    await deliver(changed('ended'));
    let told: BoundVariable | null = null;
    enabled.state.subscribe((state) => (told = state));
    request.flush({ session_id: 1, operation: 4 });
    expect(requests()).toEqual([]);
    // The subscribers got it too, before anyone reads
    expect(told).toMatchObject({ writeStatus: { kind: 'dropped' }, pending: null });
    expect(enabled.state.get()).toMatchObject({
      writeStatus: { kind: 'dropped', message: DROPPED_MESSAGE },
      pending: null,
    });
    // The result of a write that was received does not change the status field
    await deliver(operation(4, 'applied', true));
    expect(enabled.state.get().writeStatus.kind).toBe('dropped');
    enabled.close();
  });

  it('gives the state of now whoever asks first, and tells its subscribers only of changes', async () => {
    // A subscriber added before the bind reads the current status within the notification
    const inside: (string | null | undefined)[] = [];
    let enabled: VariableBinding | null = null;
    client.connection.state.subscribe(() => inside.push(enabled?.state.get().block));
    enabled = client.variables.bind(ref('engine.enabled'), { visible: false });
    const seen: BoundVariable[] = [];
    enabled.state.subscribe((state) => seen.push(state));
    await deliver(hello());
    expect(seen.map((s) => s.block)).toEqual([null]);
    // A change unrelated to variables (a log line) does not notify
    await deliver({
      type: 'event',
      session: 1,
      event: { event: 'log', seq: 0, t_ms: 5, source: 'Engine', line: 'x' },
    });
    expect(client.session.session.get()?.logs).toHaveLength(1);
    expect(seen).toHaveLength(1);
    socket().fail();
    expect(inside.at(-1)).toBe('disconnected');
    expect(seen.map((s) => s.block)).toEqual([null, 'disconnected']);
    enabled.close();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    await deliver(hello());
    // After close, no notifications and no writes
    expect(seen).toHaveLength(2);
    enabled.write(true);
    enabled.reject('no');
    expect(requests()).toEqual([]);
  });
});
