import { GuiClient } from './client';
import { createFront, createVariableEditor, createWaveforms, type Front } from './index';
import { BATCH_MS } from './connection';
import { isHolding } from './session-state';
import { FakeHttp, FakeRequest } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { httpExamples, recordedLauncherEvents } from './testing/protocol-examples';
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

/** The launcher's state of a run that waits before its first step (as the summary carries it). */
const holdingState = {
  seq: 5,
  rev: 5,
  phase: 'waiting',
  paused: true,
  holding: true,
  speed: 'max',
  pacing: 'not-paced',
  stop: { requests: 0, by: null },
  scenario: false,
  allowed: { write: { ok: true, reason: null, message: null, caution: null } },
  outside: [],
};

/** The launcher's state of a running run that takes writes. */
const runningState = { ...holdingState, phase: 'running', paused: false, holding: false };

/** A `state` event of the launcher (rev and seq from `seq`). */
const launcherEvent = (seq: number, state: object, session = 1) => ({
  type: 'event',
  session,
  event: { ...state, event: 'state', seq: 100 + seq, rev: 100 + seq, t_ms: seq },
});

const hello = (state = 'ready', id = 1, session: object = {}) => ({
  type: 'hello',
  vehicle_sil_gui: 3,
  version: '0.1.0',
  instance: 'x',
  session: {
    id,
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: variables(state),
    launcher_state: runningState,
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
  let front: Front;

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    http = new FakeHttp();
    front = createFront({
      baseUrl: 'http://127.0.0.1:8765/',
      token: 'secret',
      socket: fake.factory,
      http: http.transport,
      onError: (error) => {
        throw error;
      },
    });
    client = front.low;
    client.connect();
    socket().open();
  });

  afterEach(() => {
    front.close();
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

  it('preserves selected histories when signals change and clears them for another run', async () => {
    await deliver(hello());
    const waves = createWaveforms(client, { history: 3 });
    const demand = ref('engine.demand');
    const speed = ref('engine.speed');
    waves.setSignals([demand]);
    await deliver(values(10, false, { 'engine.demand': 11 }));
    waves.setSignals([speed, demand, speed]);
    expect(waves.series.get()).toEqual([
      { ref: speed, points: [] },
      { ref: demand, points: [{ sampleMs: 10, value: 11 }] },
    ]);
    waves.setSignals([demand]);
    expect(waves.series.get()[0].points).toHaveLength(1);
    const unsubscribe = client.session.session.subscribe((session) => {
      if (session?.id === 2) expect(waves.series.get()[0].points).toEqual([]);
    });
    await deliver(hello('ready', 2));
    unsubscribe();
    expect(waves.series.get()[0].points).toEqual([]);
    waves.close();
    waves.setSignals([speed]);
    expect(waves.series.get()).toEqual([]);
  });

  it('waits for Boolean readback, rejects invalid edits, and forgets waits from earlier runs', async () => {
    await deliver(hello(), { type: 'synced', session: 1, last_seq: null });
    const editor = createVariableEditor(front, ref('engine.enabled'));
    editor.setVisible(true);
    await deliver(values(10, false, { 'engine.enabled': false }));
    expect(editor.write(1)).toBe(false);
    http.expectNone('api/write');
    expect(editor.write(true)).toBe(true);
    expect(editor.state.get()).toMatchObject({ shown: true, waiting: true, canWrite: false });
    http.expectOne('api/write').flush({ session_id: 1, operation: 4 });
    await deliver(operation(4, 'applied', true));
    expect(editor.state.get()).toMatchObject({ shown: false, waiting: true });
    await deliver(values(20, false, { 'engine.enabled': true }));
    expect(editor.state.get()).toMatchObject({ shown: true, waiting: false });
    client.close();
    client.connect();
    socket().open();
    expect(editor.write(false)).toBe(false);
    http.expectNone('api/write');
    await deliver(hello(), { type: 'synced', session: 1, last_seq: null });
    expect(editor.write(false)).toBe(true);
    http.expectOne('api/write').flush({ session_id: 1, operation: 5 });
    await deliver(operation(5, 'applied', true));
    expect(editor.state.get().waiting).toBe(true);
    await deliver(hello('ready', 2));
    expect(editor.state.get().waiting).toBe(false);
    editor.close();
    expect(editor.write(true)).toBe(false);
  });

  it('cancels the edit if a sending subscriber closes the connection', async () => {
    await deliver(hello(), { type: 'synced', session: 1, last_seq: null });
    const editor = createVariableEditor(front, ref('engine.demand'));
    editor.setVisible(true);
    const unsubscribe = editor.state.subscribe((state) => {
      if (!state.waiting) return;
      unsubscribe();
      client.close();
    });
    expect(editor.write(25)).toBe(true);
    http.expectNone('api/write');
    expect(editor.state.get()).toMatchObject({
      waiting: false,
      canWrite: false,
      view: { write_status: { kind: 'dropped' } },
    });
    editor.close();
  });

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

  describe('availability', () => {
    const told = (peers: object, session = 1) => ({ type: 'availability', session, peers });
    const withAvailability = (given: Record<string, string>) => ({
      state: 'ready',
      reason: null,
      peers: [
        {
          name: 'Engine',
          connected: true,
          variables: SCHEMA.map((v) => ({
            ...v,
            ...(v.name in given ? { availability: given[v.name] } : {}),
          })),
        },
      ],
      peers_omitted: 0,
      variables_omitted: 0,
      operations: [],
    });

    it('is active without the field (an older server) and for a value it does not know', async () => {
      const demand = client.variables.bind(ref('engine.demand'));
      expect(demand.state.get().availability).toBe('unknown');
      await deliver(hello(), told({}));
      expect(demand.state.get().availability).toBe('active');
      await deliver({
        type: 'variables',
        session: 1,
        variables: withAvailability({ 'engine.demand': 'sometimes' }),
      });
      expect(demand.state.get().availability).toBe('active');
      demand.close();
    });

    it('is replaced by the summary and, per participant, by the notification', async () => {
      const demand = client.variables.bind(ref('engine.demand'));
      const speed = client.variables.bind(ref('engine.speed'));
      await deliver({
        ...hello(),
        session: {
          ...hello().session,
          variables: withAvailability({
            'engine.demand': 'unknown',
            'engine.speed': 'unknown',
          }),
        },
      });
      expect([demand, speed].map((b) => b.state.get().availability)).toEqual([
        'unknown',
        'unknown',
      ]);
      await deliver(told({ Engine: { 'engine.demand': 'inactive', 'engine.speed': 'active' } }));
      expect([demand, speed].map((b) => b.state.get().availability)).toEqual([
        'inactive',
        'active',
      ]);
      // What the participant reports replaces its earlier states: a variable not listed is active again
      await deliver(told({ Engine: { 'engine.speed': 'inactive' } }));
      expect([demand, speed].map((b) => b.state.get().availability)).toEqual([
        'active',
        'inactive',
      ]);
      // A participant that is not listed, one that is not known, and another run's notification change nothing
      await deliver(told({ Other: { 'engine.demand': 'inactive' } }), told({ Engine: {} }, 2));
      expect([demand, speed].map((b) => b.state.get().availability)).toEqual([
        'active',
        'inactive',
      ]);
      // A new summary replaces them all
      await deliver(changed());
      expect([demand, speed].map((b) => b.state.get().availability)).toEqual(['active', 'active']);
      demand.close();
      speed.close();
    });

    it('stays what the ECU said when the connection or the ECU is lost', async () => {
      const demand = client.variables.bind(ref('engine.demand'));
      await deliver(hello(), told({ Engine: { 'engine.demand': 'inactive' } }));
      // The ECU left the Control Server: the summary keeps the state
      const gone = withAvailability({ 'engine.demand': 'inactive' });
      gone.peers[0].connected = false;
      await deliver({ type: 'variables', session: 1, variables: gone });
      expect(demand.state.get().availability).toBe('inactive');
      // The connection between the client and the server dropped
      socket().fail();
      expect(demand.state.get().availability).toBe('inactive');
      demand.close();
    });

    it("goes back to unknown when another run starts, then follows that run's summary", async () => {
      const demand = client.variables.bind(ref('engine.demand'));
      await deliver(hello(), told({ Engine: { 'engine.demand': 'inactive' } }));
      expect(demand.state.get().availability).toBe('inactive');
      await deliver({ type: 'session', session: { ...hello().session, id: 2 } });
      expect(demand.state.get().availability).toBe('unknown');
      await deliver({
        type: 'variables',
        session: 2,
        variables: withAvailability({ 'engine.demand': 'inactive', 'engine.speed': 'active' }),
      });
      expect(demand.state.get().availability).toBe('inactive');
      demand.close();
    });

    it('leaves the list of variables as it is (a notification replaces only the states)', async () => {
      await deliver(hello());
      const peers = client.variables.state.get().peers;
      const first = peers[0]!.variables;
      await deliver(told({ Engine: { 'engine.demand': 'inactive' } }));
      const state = client.variables.state.get();
      expect(state.peers).toBe(peers);
      expect(state.peers[0]!.variables).toBe(first);
      expect(state.availability.get('Engine')?.get('engine.demand')).toBe('inactive');
    });

    it('does not change whether the variable can be written, or what it says of the value', async () => {
      const demand = client.variables.bind(ref('engine.demand'));
      await deliver(hello(), values(100, false, { 'engine.demand': 30 }));
      const before = demand.state.get();
      expect(before.block).toBeNull();
      await deliver(told({ Engine: { 'engine.demand': 'inactive' } }));
      const after = demand.state.get();
      expect(after.availability).toBe('inactive');
      expect({ ...after, availability: before.availability }).toEqual(before);
      expect(after.block).toBeNull();
      demand.close();
    });
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
    await deliver(
      hello('ready', 2, { start_paused: true, phase: 'waiting', launcher_state: holdingState }),
    );
    expect(isHolding(client.session.session.get())).toBe(true);
    expect(mode.state.get().block).toBeNull();
    mode.write('on');
    expect(requests().map((r) => r.request.body)).toEqual([
      { session: 2, writes: { Engine: { 'engine.mode': 'on' } } },
    ]);
    mode.close();
  });

  it("blocks a write with the launcher's reason, and only while the launcher refuses it", async () => {
    const enabled = client.variables.bind(ref('engine.enabled'));
    const scenario = {
      file: 'scenarios/a.yaml',
      name: null,
      step_count: null,
      steps: [],
      steps_omitted: 0,
      status: null,
      over_at_ms: null,
      timeline_url: null,
    };
    const playing =
      'this run plays a scenario, and until it ends only the scenario writes variables';
    const launcherState = (write: object) => ({
      ...holdingState,
      phase: 'running',
      paused: false,
      holding: false,
      scenario: true,
      allowed: { write },
    });
    await deliver(
      hello('ready', 1, {
        scenario,
        launcher_state: launcherState({
          ok: false,
          reason: 'scenario-running',
          message: playing,
          caution: null,
        }),
      }),
    );
    expect(enabled.state.get()).toMatchObject({
      block: 'launcher',
      launcherReason: { code: 'scenario-running', message: playing },
    });
    enabled.write(true);
    expect(requests()).toEqual([]);
    // The client keeps no rule of its own: a scenario that the launcher lets a client write to is writable
    await deliver(
      hello('ready', 1, {
        scenario,
        launcher_state: {
          ...launcherState({ ok: true, reason: null, message: null, caution: null }),
          rev: 6,
          seq: 6,
        },
      }),
    );
    expect(client.session.session.get()?.scenario).not.toBeNull();
    expect(enabled.state.get()).toMatchObject({ block: null, launcherReason: null });
    enabled.write(true);
    expect(requests()).toHaveLength(1);
    // The variable's own reason comes before the launcher's
    const speed = client.variables.bind(ref('engine.speed'));
    await deliver(
      hello('ready', 1, {
        scenario,
        launcher_state: {
          ...launcherState({
            ok: false,
            reason: 'scenario-running',
            message: playing,
            caution: null,
          }),
          rev: 7,
          seq: 7,
        },
      }),
    );
    expect(speed.state.get()).toMatchObject({ block: 'read-only', launcherReason: null });
    speed.close();
    enabled.close();
  });

  it('does not write until the launcher has said that it takes writes', async () => {
    const enabled = client.variables.bind(ref('engine.enabled'));
    await deliver(hello('ready', 1, { launcher_state: null }));
    expect(enabled.state.get()).toMatchObject({
      block: 'launcher',
      launcherReason: {
        code: 'unsupported',
        message: 'the launcher has not said yet what it accepts',
      },
    });
    enabled.write(true);
    expect(requests()).toEqual([]);
    await deliver(launcherEvent(1, runningState));
    expect(enabled.state.get().block).toBeNull();
    enabled.write(true);
    expect(requests()).toHaveLength(1);
    enabled.close();
  });

  it("has the launcher's answer before the variables can be ready (a recorded scenario run)", async () => {
    // The launcher's events of a recorded run, in their order, as the GUI server passes them on. The variables can be
    // ready only once the GUI server has connected to the Control Server, which it does after the `control_server`
    // event: by then a `state` has said what the launcher takes.
    const events = await recordedLauncherEvents('control-scenario-slow');
    const until = events.findIndex((e) => e.event === 'control_server');
    expect(until).toBeGreaterThan(0);
    expect(events.slice(0, until).some((e) => e.event === 'state')).toBe(true);

    const enabled = client.variables.bind(ref('engine.enabled'));
    await deliver(hello('connecting', 1, { launcher_state: null }));
    for (const [i, out] of events.slice(0, until + 1).entries()) {
      await deliver({ type: 'event', session: 1, event: { ...out, seq: i + 1, t_ms: i } });
    }
    await deliver(changed('ready'));
    expect(enabled.state.get()).toMatchObject({
      block: 'launcher',
      launcherReason: { code: 'scenario-running' },
    });
    enabled.close();
  });

  it('does not send a value kept back once the launcher stops taking writes', async () => {
    const enabled = client.variables.bind(ref('engine.enabled'));
    await deliver(hello());
    enabled.write(true);
    enabled.write(false); // kept back while the first write is on its way
    const [request] = requests();
    await deliver(
      launcherEvent(9, {
        ...runningState,
        allowed: {
          write: {
            ok: false,
            reason: 'scenario-running',
            message: 'a scenario plays',
            caution: null,
          },
        },
      }),
    );
    request.flush({ session_id: 1, operation: 4 });
    await vi.advanceTimersByTimeAsync(0);
    expect(requests()).toEqual([]);
    expect(enabled.state.get().writeStatus).toEqual({ kind: 'dropped', message: DROPPED_MESSAGE });
    expect(DROPPED_MESSAGE).toContain('the launcher does not take writes now');
    enabled.close();
  });

  it("shows the server's refusal of a write during a scenario as its code", async () => {
    const example = (await httpExamples()).find((e) => e.name === 'error-scenario-running');
    const enabled = client.variables.bind(ref('engine.enabled'));
    await deliver(hello());
    enabled.write(true);
    const [request] = requests();
    expect(example?.status).toBe(409);
    request.flush(example?.response, { status: 409, statusText: 'Conflict' });
    await vi.advanceTimersByTimeAsync(0);
    expect(enabled.state.get().writeStatus).toMatchObject({
      kind: 'refused',
      code: 'scenario-running',
    });
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
