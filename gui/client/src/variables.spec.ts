import { createClient } from './client';
import { BATCH_MS } from './connection';
import { HttpError } from './http';
import { isHolding } from './session-state';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { WebSocketExample, httpExamples, webSocketExamples } from './testing/protocol-examples';
import {
  HISTORY_MAX,
  HISTORY_TOTAL_MAX,
  OPERATIONS_KEPT,
  WriteError,
  readOperation,
  readVariables,
} from './variables';

const ENGINE = { peer: 'Engine', name: 'engine.demand' };
const SPEED = { peer: 'Engine', name: 'engine.speed' };

/** A client connected to a fake connection and fake HTTP. */
function setup() {
  const sockets = new FakeWebSockets();
  const http = new FakeHttp();
  const errors: unknown[] = [];
  const client = createClient({
    baseUrl: 'http://127.0.0.1:8765/',
    token: 'secret',
    socket: sockets.factory,
    http: http.transport,
    onError: (error) => errors.push(error),
  });
  client.connect();
  const socket = () => sockets.sockets[sockets.sockets.length - 1];
  socket().open();
  /** Delivers a notification from the server (waits until the batch is passed on). */
  const deliver = async (...messages: unknown[]) => {
    for (const message of messages) socket().send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
  };
  /** The watch and unwatch the client sent (authentication excluded). */
  const sent = (s: FakeSocket = socket()) =>
    s.sent.filter((m) => (m as { type: string }).type !== 'auth');
  return { client, sockets, socket, http, errors, deliver, sent };
}

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
  allowed: {},
  outside: [],
};

const summary = (id: string | number, variables?: unknown) => ({
  id: String(id),
  state: 'running',
  launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
  stop: 'none',
  ...(variables === undefined ? {} : { variables }),
});
const hello = (id: string | number | null, variables?: unknown) => ({
  type: 'hello',
  vehicle_sil_gui: 4,
  version: '0.1.0',
  instance: 'x',
  session: id === null ? null : summary(id, variables),
  replay: null,
});
const ready = (operations: unknown[] = [], more: object = {}) => ({
  state: 'ready',
  reason: null,
  peers: [
    {
      name: 'Engine',
      connected: true,
      variables: [
        {
          name: 'engine.demand',
          type: 'real',
          role: 'input',
          writable: true,
          builtin: false,
          unit: '%',
          min: 0,
          max: 100,
          step: null,
          description: '',
          enum: null,
        },
      ],
    },
  ],
  peers_omitted: 0,
  variables_omitted: 0,
  operations,
  ...more,
});
/** One entry of operations in a summary (an operation notification without type and session). */
const listed = (seq: number, state: string, done: boolean) => {
  const sent = operation(seq, state, done);
  return { seq, state: sent.state, done: sent.done, peers: sent.peers };
};
const values = (
  id: number,
  simMs: number,
  paused: boolean,
  peers: Record<string, unknown>,
  session: string | number = '1',
) => ({
  type: 'values',
  id,
  watch_id: id,
  run_id: String(session),
  sim_ms: simMs,
  paused,
  peers,
});
const engine = (sampleMs: number, demand: unknown, speed?: unknown) => ({
  Engine: {
    sample_ms: sampleMs,
    values: { 'engine.demand': demand, ...(speed === undefined ? {} : { 'engine.speed': speed }) },
  },
});
const operation = (
  seq: number,
  state: string,
  done: boolean,
  peer: object = {},
  session: string | number = '1',
) => ({
  type: 'operation',
  run_id: String(session),
  seq,
  state,
  done,
  peers: { Engine: { state, applied_at_ms: null, late: false, reason: null, ...peer } },
});

describe('readOperation', () => {
  it('says which writes took effect, and null for what is absent or not a position', () => {
    const peers = (p: object) =>
      readOperation(operation(3, 'rejected', true, p), '1')?.peers['Engine'];
    expect(peers({ applied_writes: 1, failed_write: 1 })).toMatchObject({
      appliedWrites: 1,
      failedWrite: 1,
    });
    expect(peers({ applied_writes: 0 })).toMatchObject({ appliedWrites: 0, failedWrite: null });
    expect(peers({ applied_writes: -1, failed_write: 'x' })).toMatchObject({
      appliedWrites: null,
      failedWrite: null,
    });
    expect(peers({})).toMatchObject({ appliedWrites: null, failedWrite: null }); // an older server
  });
});

describe('variables', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('reads the state from hello and from the variables messages, not from session messages', async () => {
    const { client, deliver } = setup();
    expect(client.variables.state.get().status).toBe('unknown');
    await deliver(hello(null));
    expect(client.variables.state.get()).toMatchObject({ session: null, status: 'off' });
    await deliver(hello(1, { ...ready(), state: 'connecting' }));
    expect(client.variables.state.get()).toMatchObject({
      session: '1',
      status: 'connecting',
      peers: [{}],
    });
    await deliver({ type: 'variables', run_id: '1', variables: ready() });
    const state = client.variables.state.get();
    expect(state.status).toBe('ready');
    expect(state.peers[0].variables[0]).toEqual({
      name: 'engine.demand',
      type: 'real',
      role: 'input',
      writable: true,
      builtin: false,
      unit: '%',
      min: 0,
      max: 100,
      step: null,
      description: '',
      enum: null,
    });
    // A session notification of the same run has no variables: keep the current state
    await deliver({ type: 'session', session: summary(1) });
    expect(client.variables.state.get()).toBe(state);
    // Another run started: unknown until its variables arrive
    await deliver({ type: 'session', session: summary(2) });
    expect(client.variables.state.get()).toMatchObject({
      session: '2',
      status: 'unknown',
      peers: [],
    });
    await deliver({
      type: 'variables',
      run_id: '2',
      variables: { state: 'off', reason: 'no control server' },
    });
    expect(client.variables.state.get()).toMatchObject({
      session: '2',
      status: 'off',
      reason: 'no control server',
    });
  });

  it('reads what it can of a schema and leaves the rest out', () => {
    const state = readVariables(
      {
        state: 'later-state',
        reason: 5,
        peers: [
          {
            name: 'A',
            connected: 'yes',
            variables: [
              { name: 'x', type: 'vector', enum: [{ name: 'a' }, { name: 'b', value: 1 }] },
              7,
              { type: 'real' },
            ],
          },
          'junk',
          { connected: true },
        ],
        variables_omitted: 3,
      },
      '4',
    );
    expect(state).toEqual({
      session: '4',
      status: 'later-state',
      reason: null,
      peers: [
        {
          name: 'A',
          connected: false,
          variables: [
            {
              name: 'x',
              type: 'vector',
              role: 'output',
              writable: false,
              builtin: false,
              unit: '',
              min: null,
              max: null,
              step: null,
              description: '',
              enum: [{ name: 'b', value: 1 }],
            },
          ],
        },
      ],
      availability: new Map([['A', new Map([['x', 'active']])]]),
      peersOmitted: 0,
      variablesOmitted: 3,
    });
    expect(readVariables(null, null).status).toBe('unknown');
  });

  it('preserves builtin flags for built-in and user-defined variables', () => {
    const varDef = (name: string, builtin: boolean) => ({
      name,
      type: 'integer',
      role: 'output',
      writable: false,
      builtin,
      unit: '',
      min: null,
      max: null,
      step: null,
      description: '',
      enum: null,
      availability: 'active',
    });
    const state = readVariables(
      {
        state: 'ready',
        reason: null,
        peers: [
          {
            name: 'Engine',
            connected: true,
            variables: [
              varDef('ecu.connected', true),
              varDef('peer.steps', true),
              varDef('j1939.CAN1.frames_sent', true),
              varDef('can.CAN1.frames_rx', true),
              varDef('lin.LIN1.frames_tx', true),
              varDef('eth.ETH1.frames_rx', true),
              varDef('engine.speed', false),
            ],
          },
        ],
        peers_omitted: 0,
        variables_omitted: 0,
        operations: [],
      },
      '7',
    );
    expect(state.peers[0].variables.map((v) => [v.name, v.builtin])).toEqual([
      ['ecu.connected', true],
      ['peer.steps', true],
      ['j1939.CAN1.frames_sent', true],
      ['can.CAN1.frames_rx', true],
      ['lin.LIN1.frames_tx', true],
      ['eth.ETH1.frames_rx', true],
      ['engine.speed', false],
    ]);
  });

  it('merges the watches of one variable into one subscription, at the shortest period asked', async () => {
    const { client, sent, deliver } = setup();
    await deliver(hello(1, ready()));
    const a = client.variables.watch([ENGINE, SPEED], { periodMs: 500 });
    const b = client.variables.watch([ENGINE], { periodMs: 500 });
    await vi.advanceTimersByTimeAsync(0);
    // Two watches on the same variable make one subscription (additions and removals are sent once, together)
    expect(sent()).toEqual([{ type: 'watch', id: 1, variables: [ENGINE, SPEED], period_ms: 500 }]);
    // When a watch with a shorter interval arrives, the variable moves to the shorter subscription (requested intervals
    // are rounded to the tiers)
    const c = client.variables.watch([ENGINE], { periodMs: 60 });
    await vi.advanceTimersByTimeAsync(0);
    expect(sent().slice(1)).toEqual([
      { type: 'watch', id: 2, variables: [ENGINE], period_ms: 50 },
      { type: 'watch', id: 1, variables: [SPEED], period_ms: 500 },
    ]);
    // Values reach every watch that observes the variable
    await deliver(values(2, 120, false, engine(110, 40)));
    for (const watch of [a, b, c]) {
      expect(watch.values.get()).toEqual({
        Engine: { 'engine.demand': { value: 40, sampleMs: 110, simMs: 120, paused: false } },
      });
    }
    expect(a.value(SPEED)).toBeUndefined();
    // After a stop, the subscriptions follow the remaining watches. Stopping the last one drops the subscription
    c.close();
    c.close();
    await vi.advanceTimersByTimeAsync(0);
    expect(sent().slice(3)).toEqual([
      { type: 'unwatch', id: 2 },
      { type: 'watch', id: 1, variables: [ENGINE, SPEED], period_ms: 500 },
    ]);
    a.close();
    b.close();
    await vi.advanceTimersByTimeAsync(0);
    expect(sent().slice(5)).toEqual([{ type: 'unwatch', id: 1 }]);
    // A stopped watch receives nothing more
    await deliver(values(1, 300, false, engine(290, 50)));
    expect(a.values.get()['Engine']?.['engine.demand']?.value).toBe(40);
    expect(a.value(ENGINE)).toBeUndefined();
  });

  it('asks for the periods the server takes, and for all when a watch wants all', async () => {
    const { client, sent, deliver } = setup();
    await deliver(hello(1, ready()));
    client.variables.watch([ENGINE], { periodMs: 1 }); // Never requests less than the server's lower limit
    client.variables.watch([SPEED]); // Default 100 ms
    const all = client.variables.watch('all', { periodMs: 100 });
    await vi.advanceTimersByTimeAsync(0);
    // A variable at the same interval as "all" arrives through the "all" subscription, so it is not requested by name
    expect(sent()).toEqual([
      { type: 'watch', id: 1, variables: 'all', period_ms: 100 },
      { type: 'watch', id: 2, variables: [ENGINE], period_ms: 20 },
    ]);
    await deliver(
      values(1, 10, false, {
        ...engine(0, 1, 2),
        Body: { sample_ms: 0, values: { lamp: true, t: null } },
      }),
    );
    expect(all.values.get()).toEqual({
      Engine: {
        'engine.demand': { value: 1, sampleMs: 0, simMs: 10, paused: false },
        'engine.speed': { value: 2, sampleMs: 0, simMs: 10, paused: false },
      },
      Body: {
        lamp: { value: true, sampleMs: 0, simMs: 10, paused: false },
        t: { value: null, sampleMs: 0, simMs: 10, paused: false },
      },
    });
  });

  it('subscribes again after a reconnection, also for watches made while it was not connected', async () => {
    const { client, sockets, socket, sent, deliver } = setup();
    await deliver(hello(1, ready()));
    const first = socket();
    const watch = client.variables.watch([ENGINE], { periodMs: 100 });
    await vi.advanceTimersByTimeAsync(0);
    await deliver(values(1, 10, true, engine(0, 5)));
    first.fail();
    client.variables.watch([SPEED], { periodMs: 20 }); // Added while disconnected
    await vi.advanceTimersByTimeAsync(500);
    expect(sockets.sockets.length).toBe(2);
    client.variables.watch('all', { periodMs: 1000 }); // Added while connecting
    socket().open();
    // After authentication, all current subscriptions (with the same ids)
    expect((socket().sent[0] as { type: string }).type).toBe('auth');
    expect(sent()).toEqual([
      { type: 'watch', id: 3, variables: 'all', period_ms: 1000 },
      { type: 'watch', id: 1, variables: [ENGINE], period_ms: 100 },
      { type: 'watch', id: 2, variables: [SPEED], period_ms: 20 },
    ]);
    await vi.advanceTimersByTimeAsync(0);
    expect(sent().length).toBe(3); // The same content is not resent
    // Back in the same run: the values are kept. For another run they are dropped
    await deliver(hello(1, ready()));
    expect(watch.value(ENGINE)?.value).toBe(5);
    await deliver(hello(2, ready()));
    expect(watch.values.get()).toEqual({});
    // A value of the previous run arriving late is not applied
    await deliver(values(1, 20, false, engine(10, 6), 1));
    expect(watch.values.get()).toEqual({});
  });

  it('keeps a bounded history per variable, in sample order, for the run it belongs to', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    const short = client.variables.watch([ENGINE], { history: 3 });
    const long = client.variables.watch([ENGINE, SPEED], { history: 5 });
    const none = client.variables.watch([ENGINE]);
    for (let n = 1; n <= 7; n++)
      await deliver(values(1, n * 10, false, engine(n * 10, n, n % 2 === 0)));
    // A value of the same step arriving again (while paused and so on) adds no point
    await deliver(values(1, 70, true, engine(70, 7, false)));
    expect(short.history(ENGINE)).toEqual([
      { sampleMs: 50, value: 5 },
      { sampleMs: 60, value: 6 },
      { sampleMs: 70, value: 7 },
    ]);
    expect(long.history(ENGINE).map((p) => p.value)).toEqual([3, 4, 5, 6, 7]);
    expect(long.history(SPEED).map((p) => p.value)).toEqual([false, true, false, true, false]);
    expect(short.history(SPEED)).toEqual([]);
    expect(none.history(ENGINE)).toEqual([]);
    expect(short.value(ENGINE)).toEqual({ value: 7, sampleMs: 70, simMs: 70, paused: true });
    // A non-finite value (arriving as null) is one point too
    await deliver(values(1, 80, false, engine(80, null)));
    expect(short.history(ENGINE)[2]).toEqual({ sampleMs: 80, value: null });
    // Empty again once the run changes
    await deliver({ type: 'session', session: summary(2) });
    expect(long.history(ENGINE)).toEqual([]);
    await deliver(
      { type: 'variables', run_id: '2', variables: ready() },
      values(1, 5, false, engine(0, 9), 2),
    );
    expect(long.history(ENGINE)).toEqual([{ sampleMs: 0, value: 9 }]);
    // A stopped watch returns no history. The history of a variable nobody requests history for any more is dropped
    short.close();
    long.close();
    expect(short.history(ENGINE)).toEqual([]);
    const again = client.variables.watch([ENGINE], { history: HISTORY_MAX * 10 });
    expect(again.history(ENGINE)).toEqual([]);
    for (let n = 1; n <= 3; n++) await deliver(values(1, n, false, engine(n, n), 2));
    expect(again.history(ENGINE).length).toBe(3);
  });

  it('does not merge an older hello operation into a newer run with the same sequence', async () => {
    const { client, http, deliver } = setup();
    let written: Promise<unknown> | null = null;
    let started = false;
    const unsubscribe = client.variables.state.subscribe((state) => {
      if (started || state.session !== '2') return;
      started = true;
      written = client.variables.write({ Engine: { 'engine.demand': 40 } });
      http.expectOne('api/write').flush({ run_id: '2', operation: 7 });
    });
    await deliver(
      hello(1, ready([listed(7, 'rejected', true)])),
      { type: 'session', session: summary(2) },
      { type: 'variables', run_id: '2', variables: ready() },
      operation(7, 'applied', true, { applied_at_ms: 20 }, 2),
    );
    expect(started).toBe(true);
    await expect(written).resolves.toMatchObject({ session: '2', seq: 7, state: 'applied' });
    expect(client.variables.operations.get()).toMatchObject([
      { session: '2', seq: 7, state: 'applied' },
    ]);
    unsubscribe();
    client.close();
  });

  it('accepts a same-batch variables snapshot after a synchronous run response', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const started = client.run.start({ file: 'a.yaml', disable: [] });
    const request = http.expectOne('api/run');
    let answered = false;
    const unsubscribe = client.session.session.subscribe((session) => {
      if (answered || session?.id !== '2') return;
      answered = true;
      request.flush({ instance: 'x', run_id: '2' });
    });
    await deliver(
      { type: 'session', session: summary(2) },
      { type: 'variables', run_id: '2', variables: ready() },
    );
    await started;
    expect(answered).toBe(true);
    expect(client.variables.state.get()).toMatchObject({ session: '2', status: 'ready' });
    expect(client.variables.state.get().peers[0]?.variables[0]?.name).toBe('engine.demand');
    unsubscribe();
    client.close();
  });

  it('clears readings and history when a new server reuses the session id', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    const watch = client.variables.watch([ENGINE], { history: 10 });
    await deliver(values(1, 10, false, engine(10, 4)));
    expect(watch.history(ENGINE)).toHaveLength(1);
    let historyDuringSessionNotification: unknown = null;
    const unsubscribe = client.session.session.subscribe(() => {
      historyDuringSessionNotification = watch.history(ENGINE);
    });
    await deliver({ ...hello(1, ready()), instance: 'new-server' });
    expect(historyDuringSessionNotification).toEqual([]);
    unsubscribe();
    expect(watch.history(ENGINE)).toEqual([]);
    expect(watch.value(ENGINE)).toBeUndefined();
    await deliver(values(1, 20, false, engine(20, 8)));
    expect(watch.history(ENGINE)).toEqual([{ sampleMs: 20, value: 8 }]);
  });

  it('accepts a smaller session id after the server instance changes', async () => {
    const { client, deliver } = setup();
    await deliver(hello(9, ready()));
    await deliver({ ...hello(1, ready()), instance: 'new-server' });
    expect(client.variables.state.get()).toMatchObject({ session: '1', status: 'ready' });
    client.close();
  });

  it('resolves a write when the operation is applied, whichever comes first, the answer or the result', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const first = client.variables.write(
      { Engine: { 'engine.demand': 40 } },
      { atMs: 100, late: 'apply' },
    );
    const request = http.expectOne('api/write');
    expect(request.request.body).toEqual({
      run_id: '1',
      instance: 'x',
      writes: { Engine: { 'engine.demand': 40 } },
      at_ms: 100,
      late: 'apply',
    });
    request.flush({ run_id: '1', operation: 7 });
    await deliver(operation(7, 'sent', false), operation(7, 'queued', false));
    expect(client.variables.operations.get().map((o) => o.state)).toEqual(['queued']);
    await deliver(
      operation(7, 'applied', true, { applied_at_ms: 100, late: true, applied_writes: 1 }),
    );
    await expect(first).resolves.toEqual({
      session: '1',
      seq: 7,
      state: 'applied',
      done: true,
      peers: {
        Engine: {
          state: 'applied',
          appliedAtMs: 100,
          late: true,
          reason: null,
          appliedWrites: 1,
          failedWrite: null,
        },
      },
    });
    // The result arrived before the HTTP answer
    const second = client.variables.write({ Engine: { 'engine.demand': 41 } });
    await deliver(operation(8, 'applied', true, { applied_at_ms: 110 }));
    http.expectOne('api/write').flush({ run_id: '1', operation: 8 });
    await expect(second).resolves.toMatchObject({ seq: 8, state: 'applied' });
    // A settled operation is not set back by an old state arriving late
    await deliver(operation(8, 'queued', false));
    expect(client.variables.operations.get().map((o) => [o.seq, o.state])).toEqual([
      [7, 'applied'],
      [8, 'applied'],
    ]);
    http.verify();
  });

  it('tells when the server queued a write via enqueue(), before its result, and not when it refused it', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const queuedPromise = client.variables.enqueue({ Engine: { 'engine.demand': 40 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 7 });
    const queued = await queuedPromise;
    expect(queued).toMatchObject({ session: '1', seq: 7 });
    const applied = queued.applied();
    await deliver(operation(7, 'applied', true, { applied_at_ms: 100 }));
    await expect(applied).resolves.toMatchObject({ seq: 7 });

    // Refused write rejects enqueue() directly
    const refused = client.variables.enqueue({ Engine: { 'engine.demand': 400 } });
    http
      .expectOne('api/write')
      .flush(
        { error: { code: 'invalid-field', message: 'out of range', detail: null } },
        { status: 400, statusText: 'Bad Request' },
      );
    await expect(refused).rejects.toMatchObject({ kind: 'refused', code: 'invalid-field' });
  });

  it('rejects a write that was refused, not applied, or not answered, and says which', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const refusedExample = (await httpExamples()).find((e) => e.name === 'error-not-writable');
    const refused = client.variables.write({ Engine: { 'engine.speed': 900 } });
    http
      .expectOne('api/write')
      .flush(refusedExample?.response, { status: 400, statusText: 'Bad Request' });
    await expect(refused).rejects.toMatchObject({
      name: 'WriteError',
      kind: 'refused',
      code: 'not-writable',
      operation: null,
    });

    const failed = client.variables.write({ Engine: { 'engine.demand': 1 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 3 });
    const caught = failed.catch((e: unknown) => e);
    await deliver(operation(3, 'rejected', true, { reason: 'the setter failed' }));
    const error = (await caught) as WriteError;
    expect(error).toBeInstanceOf(WriteError);
    expect(error).toMatchObject({ kind: 'failed', operation: { seq: 3, state: 'rejected' } });
    expect(error.message).toBe(
      'the write (operation 3) was not applied: Engine: rejected (the setter failed)',
    );

    // No answer came: unknown whether it was applied (with the last known state attached)
    const silent = client.variables.write({ Engine: { 'engine.demand': 2 } }, { timeoutMs: 3000 });
    http.expectOne('api/write').flush({ run_id: '1', operation: 4 });
    const waited = silent.catch((e: unknown) => e);
    await deliver(operation(4, 'queued', false));
    await vi.advanceTimersByTimeAsync(3000);
    expect(await waited).toMatchObject({
      kind: 'no-answer',
      operation: { seq: 4, state: 'queued' },
    });
    expect(((await waited) as WriteError).message).toContain(
      'no result of the write (operation 4) came in 3 s',
    );
    // A result arriving after the timeout still goes into the list of operations
    await deliver(operation(4, 'applied', true));
    expect(client.variables.operations.get().find((o) => o.seq === 4)?.state).toBe('applied');

    // No HTTP answer: unknown whether the server received it
    const lost = client.variables.write({ Engine: { 'engine.demand': 3 } });
    http.expectOne('api/write').fail(HttpError.noAnswer('POST', 'api/write', 30000));
    await expect(lost).rejects.toMatchObject({ kind: 'no-answer', code: null });

    // The run changed while waiting, or the client was closed
    const old = client.variables.write({ Engine: { 'engine.demand': 4 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 5 });
    const replaced = old.catch((e: unknown) => e);
    await deliver({ type: 'session', session: summary(2) });
    expect(await replaced).toMatchObject({ kind: 'no-answer' });
    const open = client.variables.write({ Engine: { 'engine.demand': 5 } });
    http.expectOne('api/write').flush({ run_id: '2', operation: 6 });
    const closed = open.catch((e: unknown) => e);
    await vi.advanceTimersByTimeAsync(0);
    client.close();
    expect(await closed).toMatchObject({ kind: 'closed' });
    http.verify();
  });

  it('learns the result of a write from the summary after a reconnection', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    const write = client.variables.write({ Engine: { 'engine.demand': 40 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 9 });
    socket().fail(); // The result notification was missed
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // operations of the summary are operation notifications without type and session
    const sent = operation(9, 'applied', true, { applied_at_ms: 0 });
    const kept = { seq: sent.seq, state: sent.state, done: sent.done, peers: sent.peers };
    await deliver(hello(1, ready([kept])));
    await expect(write).resolves.toMatchObject({ seq: 9, state: 'applied' });
  });

  it('writes to the run it sees, and gives up a write whose answer comes after the run changed', async () => {
    const { client, http, deliver } = setup();
    // The run is not known yet: refused without sending
    await expect(client.variables.write({ Engine: { 'engine.demand': 1 } })).rejects.toMatchObject({
      kind: 'refused',
      code: 'not-running',
    });
    http.expectNone('api/write');
    await deliver(hello(1, ready()));
    const enqueued = client.variables
      .enqueue({ Engine: { 'engine.demand': 2 } })
      .catch((e: unknown) => e);
    const request = http.expectOne('api/write');
    expect(request.request.body).toMatchObject({ run_id: '1' });
    // The next run started before the answer
    await deliver({ type: 'session', session: summary(2) });
    expect(await enqueued).toMatchObject({ kind: 'no-answer' });
    // A late answer for the previous run: ignored
    request.flush({ run_id: '1', operation: 3 });
    await vi.advanceTimersByTimeAsync(0);
    // The next write is aimed at the new run
    const next = client.variables.write({ Engine: { 'engine.demand': 3 } }).catch(() => undefined);
    expect(http.expectOne('api/write').request.body).toMatchObject({ run_id: '2' });
    client.close();
    await next;
  });

  it('gives up a write whose result left the summary while the connection was down, not a newer one', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    const lost = client.variables
      .write({ Engine: { 'engine.demand': 40 } })
      .catch((e: unknown) => e);
    http.expectOne('api/write').flush({ run_id: '1', operation: 3 });
    await deliver(operation(3, 'queued', false));
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    // A write sent before the reconnect has a higher number than the operations in the summary
    const newer = client.variables.write({ Engine: { 'engine.demand': 41 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 80 });
    socket().open();
    // Meanwhile 3 settled and more operations followed than the server puts in a summary: 3 is not in the summary and
    // is older than the boundary (40)
    const kept = [];
    for (let seq = 40; seq < 72; seq++) kept.push(listed(seq, 'applied', true));
    await deliver(hello(1, ready(kept, { operations_from_seq: 40, operations_open_omitted: 0 })));
    const error = (await lost) as WriteError;
    expect(error).toMatchObject({ kind: 'no-answer' });
    expect(error.message).toContain('was lost while the connection was down');
    await deliver(operation(80, 'applied', true));
    await expect(newer).resolves.toMatchObject({ seq: 80 });
  });

  it('keeps waiting for a write that is still open after a reconnection, however many came after it', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    // A write while paused: does not settle until time advances
    let outcome: unknown = 'waiting';
    const write = client.variables.write({ Engine: { 'engine.demand': 40 } });
    write.then(
      (o) => (outcome = o),
      (e: unknown) => (outcome = e),
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    await deliver(operation(1, 'queued', false));
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // 39 operations followed meanwhile. The summary has the unsettled 1 and the last 32 (9 to 40). The boundary is 9
    const kept = [listed(1, 'queued', false)];
    for (let seq = 9; seq <= 40; seq++) kept.push(listed(seq, 'applied', true));
    const again = ready(kept, { operations_from_seq: 9, operations_open_omitted: 0 });
    await deliver(hello(1, again));
    expect(outcome).toBe('waiting');
    await deliver(operation(1, 'applied', true));
    await expect(write).resolves.toMatchObject({ seq: 1, state: 'applied' });
  });

  it('does not guess from a summary without the boundary, and takes a result out of a long summary', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    let first: unknown = 'waiting';
    client.variables.write({ Engine: { 'engine.demand': 1 } }).then(
      (o) => (first = o),
      (e: unknown) => (first = e),
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 2 });
    const second = client.variables.write({ Engine: { 'engine.demand': 2 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 3 });
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // A summary without the boundary: 2 is not in the list, but whether it settled is unknown, so keep waiting. 3
    // settled and heads the list, followed by more than OPERATIONS_KEPT unsettled operations (cut from the local list)
    const kept = [listed(3, 'applied', true)];
    for (let seq = 4; seq < 4 + OPERATIONS_KEPT + 5; seq++) kept.push(listed(seq, 'queued', false));
    await deliver(hello(1, ready(kept)));
    await expect(second).resolves.toMatchObject({ seq: 3, state: 'applied' });
    expect(client.variables.operations.get().some((o) => o.seq === 3)).toBe(false);
    expect(first).toBe('waiting');
    client.close();
    await vi.advanceTimersByTimeAsync(0);
    expect(first).toMatchObject({ kind: 'closed' });
  });

  it('says so when the server does not keep track of an open write any more', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const write = client.variables
      .write({ Engine: { 'engine.demand': 40 } })
      .catch((e: unknown) => e);
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    const kept = [listed(5, 'queued', false), listed(6, 'queued', false)];
    await deliver(hello(1, ready(kept, { operations_from_seq: 5, operations_open_omitted: 4 })));
    const error = (await write) as WriteError;
    expect(error).toMatchObject({ kind: 'no-answer' });
    expect(error.message).toContain('does not keep track of the write (operation 1)');
  });

  it('checks a write against the summary that came while its answer was on the way', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    const accepted: number[] = [];
    const write = (value: number) =>
      client.variables
        .enqueue({ Engine: { 'engine.demand': value } })
        .then((q) => {
          accepted.push(q.seq);
          return q.applied();
        })
        .catch((e: unknown) => e);
    const gone = write(1);
    const done = write(2);
    const open = write(3);
    const requests = http.requests.splice(0);
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // Reconnected before the answer: 3 settled and left the summary, 4 settled and is in the summary, 5 is unsettled
    const kept = [listed(4, 'applied', true), listed(5, 'queued', false)];
    await deliver(hello(1, ready(kept, { operations_from_seq: 4, operations_open_omitted: 0 })));
    requests[0].flush({ run_id: '1', operation: 3 });
    requests[1].flush({ run_id: '1', operation: 4 });
    requests[2].flush({ run_id: '1', operation: 5 });
    expect(await gone).toMatchObject({ kind: 'no-answer' });
    expect(await done).toMatchObject({ seq: 4, state: 'applied' });
    expect(accepted).toEqual([3, 4, 5]);
    await deliver(operation(5, 'applied', true));
    expect(await open).toMatchObject({ seq: 5, state: 'applied' });
  });

  it('keeps a bounded list of operations', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    const messages = [];
    for (let seq = 1; seq <= OPERATIONS_KEPT + 10; seq++)
      messages.push(operation(seq, 'applied', true));
    await deliver(
      ...messages,
      operation(5, 'applied', true, {}, 99),
      { type: 'operation', run_id: '1' },
      'junk',
    );
    const kept = client.variables.operations.get();
    expect(kept.length).toBe(OPERATIONS_KEPT);
    expect([kept[0].seq, kept[kept.length - 1].seq]).toEqual([11, OPERATIONS_KEPT + 10]);
  });

  it('calls a run holding while it stands still before its first step, as the launcher says', async () => {
    const { client, http, deliver } = setup();
    const app = (await httpExamples()).find((e) => e.name === 'app')?.response;
    const loading = client.loadApp();
    http.expectOne('api/app').flush(app);
    await loading;
    const standing = {
      ...summary(1, ready()),
      phase: 'waiting',
      start_paused: true,
      time: null,
      result: null,
      launcher_state: holdingState,
    };
    await deliver(hello(1), { type: 'synced', run_id: '1', last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(false);
    await deliver({ ...hello(1), session: standing }, { type: 'synced', run_id: '1', last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(true);
    // The first step has begun (a resume, a step): the launcher's next state says so
    await deliver({
      type: 'event',
      run_id: '1',
      event: {
        event: 'state',
        t_ms: 0,
        ...holdingState,
        seq: 6,
        rev: 6,
        phase: 'running',
        holding: false,
      },
    });
    expect(isHolding(client.session.session.get())).toBe(false);
  });
});

describe('variables: what the review found', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('calls a write the server could not confirm no-answer, with the code, not refused', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    for (const [status, code] of [
      [504, 'write-timeout'],
      [502, 'write-failed'],
      [500, 'internal'], // The server failed midway: possibly after passing it to the launcher
    ] as const) {
      const write = client.variables
        .write({ Engine: { 'engine.demand': 1 } })
        .catch((e: unknown) => e);
      http
        .expectOne('api/write')
        .flush(
          { error: { code, message: 'no answer in 5 s', detail: null } },
          { status, statusText: '' },
        );
      const error = (await write) as WriteError;
      expect(error).toMatchObject({ kind: 'no-answer', code, operation: null });
      expect(error.message).toContain('may have been taken');
    }
    // Only what is known not to have been sent is refused
    const busy = client.variables
      .write({ Engine: { 'engine.demand': 1 } })
      .catch((e: unknown) => e);
    http
      .expectOne('api/write')
      .flush(
        { error: { code: 'write-busy', message: 'busy', detail: null } },
        { status: 503, statusText: '' },
      );
    expect(await busy).toMatchObject({ kind: 'refused', code: 'write-busy' });
  });

  it('caps huge finite timeouts, rejects non-positive/non-finite timeouts and unknown fields, and clears on close()', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    for (const bad of [Infinity, -Infinity, NaN, -5, 0]) {
      await expect(
        client.variables.write({ Engine: { 'engine.demand': 1 } }, { timeoutMs: bad }),
      ).rejects.toMatchObject({ kind: 'refused', code: 'invalid-argument' });
    }
    await expect(
      client.variables.write({ Engine: { 'engine.demand': 1 } }, {
        onAccepted: () => undefined,
      } as never),
    ).rejects.toMatchObject({ kind: 'refused', code: 'unknown-field' });
    http.expectNone('api/write');

    const outcomes: Record<string, string> = {};
    const defaultWrite = client.variables.write({ Engine: { 'engine.demand': 1 } });
    defaultWrite.then(
      () => (outcomes['default'] = 'applied'),
      (e: WriteError) => (outcomes['default'] = e.kind),
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 101 });

    const hugeWrite = client.variables.write(
      { Engine: { 'engine.demand': 1 } },
      { timeoutMs: 2 ** 40 },
    );
    hugeWrite.then(
      () => (outcomes['huge'] = 'applied'),
      (e: WriteError) => (outcomes['huge'] = e.kind),
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 102 });

    await vi.advanceTimersByTimeAsync(9_999);
    expect(outcomes).toEqual({});
    await vi.advanceTimersByTimeAsync(1);
    expect(outcomes).toEqual({ default: 'no-answer' });
    await vi.advanceTimersByTimeAsync(24 * 3600 * 1000);
    expect(outcomes).toEqual({ default: 'no-answer' });
    await deliver(operation(102, 'applied', true));
    expect(outcomes).toEqual({ default: 'no-answer', huge: 'applied' });

    const openWrite = client.variables.write(
      { Engine: { 'engine.demand': 1 } },
      { timeoutMs: 2 ** 40 },
    );
    openWrite.then(
      () => (outcomes['open'] = 'applied'),
      (e: WriteError) => (outcomes['open'] = e.kind),
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 103 });
    const flying = client.variables
      .write({ Engine: { 'engine.demand': 2 } })
      .catch((e: unknown) => e);
    client.close();
    await vi.advanceTimersByTimeAsync(0);
    expect(outcomes['open']).toBe('closed');
    expect(await flying).toMatchObject({ kind: 'closed' });
    http.expectOne('api/write').flush({ run_id: '1', operation: 999 }); // A late answer is no longer used
    expect(vi.getTimerCount()).toBe(0);
  });

  it('applies a result that came in the same batch as the next run before it gives the write up', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const write = client.variables.write({ Engine: { 'engine.demand': 1 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 5 });
    await deliver(operation(5, 'applied', true), { type: 'session', session: summary(2) });
    await expect(write).resolves.toMatchObject({ seq: 5, state: 'applied' });
    expect(client.variables.operations.get()).toEqual([]); // The new run's operations start empty
  });

  it('takes room for a history as points come, up to what was asked and to a total', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    // Size reserved in Node's ArrayBuffers (histories are kept in Float64Arrays)
    const node = globalThis as unknown as { process: { memoryUsage(): { arrayBuffers: number } } };
    const buffers = () => node.process.memoryUsage().arrayBuffers;
    const before = buffers();
    // Requesting full-size histories for every variable takes no memory until values arrive
    const all = client.variables.watch('all', { history: HISTORY_MAX });
    const named = client.variables.watch([ENGINE], { history: HISTORY_MAX });
    expect(buffers() - before).toBeLessThan(1024 * 1024);
    const many: Record<string, number> = {};
    for (let n = 0; n < 1000; n++) many[`v${n}`] = n;
    await deliver(
      values(1, 10, false, { Engine: { sample_ms: 0, values: { ...many, 'engine.demand': 1 } } }),
    );
    expect(buffers() - before).toBeLessThan(2 * 1024 * 1024);
    expect(all.history({ peer: 'Engine', name: 'v7' })).toEqual([{ sampleMs: 0, value: 7 }]);
    expect(named.history(ENGINE)).toEqual([{ sampleMs: 0, value: 1 }]);
    // When the run changes, the histories of the previous run's variables go with their memory (no growth as names keep
    // changing)
    await deliver(
      { type: 'session', session: summary(2) },
      { type: 'variables', run_id: '2', variables: ready() },
    );
    expect(all.history({ peer: 'Engine', name: 'v7' })).toEqual([]);
    await deliver(values(1, 10, false, engine(0, 3), 2));
    expect(named.history(ENGINE)).toEqual([{ sampleMs: 0, value: 3 }]);
    all.close();
    named.close();
    expect(HISTORY_TOTAL_MAX).toBeGreaterThan(HISTORY_MAX);
  });

  it('keeps fewer points than asked once the histories together are at their total', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    // Full-size histories for 40 variables: 4 million points requested, but only up to the overall limit is taken
    const refs = Array.from({ length: 40 }, (_, n) => ({ peer: 'Engine', name: `v${n}` }));
    const watch = client.variables.watch(refs, { history: HISTORY_MAX });
    const steps = HISTORY_MAX + 10;
    const entry: Record<string, number> = {};
    for (const ref of refs) entry[ref.name] = 0;
    for (let step = 0; step < steps; step += 5000) {
      const batch = [];
      for (let n = step; n < Math.min(step + 5000, steps); n++) {
        batch.push(values(1, n, false, { Engine: { sample_ms: n, values: entry } }));
      }
      await deliver(...batch);
    }
    const sizes = refs.map((ref) => watch.history(ref).length);
    expect(sizes.reduce((a, b) => a + b, 0)).toBeLessThanOrEqual(HISTORY_TOTAL_MAX);
    expect(Math.max(...sizes)).toBeLessThanOrEqual(HISTORY_MAX);
    expect(Math.min(...sizes)).toBeGreaterThan(1000); // Every variable keeps its newest points
    for (const ref of refs.slice(0, 3)) {
      const points = watch.history(ref);
      expect(points[points.length - 1]?.sampleMs).toBe(steps - 1);
    }
    watch.close();
  }, 60_000);

  it('widens a small history when a watch of everything asks for more, and takes odd sizes as none', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1, ready()));
    const small = client.variables.watch([ENGINE], { history: 2 });
    const all = client.variables.watch('all', { history: 5 });
    const none = client.variables.watch('all', { history: NaN });
    const negative = client.variables.watch([ENGINE], { history: -3 });
    for (let n = 1; n <= 6; n++) await deliver(values(1, n, false, engine(n, n)));
    expect(all.history(ENGINE).map((p) => p.value)).toEqual([2, 3, 4, 5, 6]);
    expect(small.history(ENGINE).map((p) => p.value)).toEqual([5, 6]);
    expect(none.history(ENGINE)).toEqual([]);
    expect(negative.history(ENGINE)).toEqual([]);
    // A value of the same step arrived changed while paused: replaces the last point (no point added)
    await deliver(values(1, 6, true, engine(6, 60)));
    expect(all.history(ENGINE).map((p) => p.value)).toEqual([2, 3, 4, 5, 60]);
    all.close();
    none.close();
    // The history of a variable no watch requests history for any more is removed
    small.close();
    const again = client.variables.watch([ENGINE], { history: 5 });
    expect(again.history(ENGINE)).toEqual([]);
  });

  it('asks for all when the names would make a message the server cuts the connection for', async () => {
    const { client, sent, deliver } = setup();
    await deliver(hello(1, ready()));
    const long = Array.from({ length: 1100 }, (_, n) => ({
      peer: 'Engine',
      name: 'n'.repeat(250) + n,
    }));
    const watch = client.variables.watch(long, { periodMs: 100 });
    await vi.advanceTimersByTimeAsync(0);
    expect(sent()).toEqual([{ type: 'watch', id: 1, variables: 'all', period_ms: 100 }]);
    // Of what arrives, only the watched variables are kept
    await deliver(
      values(1, 5, false, { Engine: { sample_ms: 0, values: { [long[3].name]: 3, other: 9 } } }),
    );
    expect(watch.values.get()).toEqual({
      Engine: { [long[3].name]: { value: 3, sampleMs: 0, simMs: 5, paused: false } },
    });
    // Short names are requested by name
    const short = client.variables.watch([ENGINE], { periodMs: 20 });
    await vi.advanceTimersByTimeAsync(0);
    expect(sent()[1]).toEqual({ type: 'watch', id: 2, variables: [ENGINE], period_ms: 20 });
    short.close();
    watch.close();
  });

  it('tells when the server refused a watch, and keeps to the period the server takes', async () => {
    const { client, http, deliver } = setup();
    const app = (await httpExamples()).find((e) => e.name === 'app')?.response as {
      limits: Record<string, number>;
    };
    const loading = client.loadApp();
    http
      .expectOne('api/app')
      .flush({ ...app, limits: { ...app.limits, watch_period_ms_max: 500 } });
    await loading;
    await deliver(hello(1, ready()));
    const slow = client.variables.watch([ENGINE], { periodMs: 60_000 });
    await vi.advanceTimersByTimeAsync(0);
    expect(client.variables.watchProblem.get()).toBeNull();
    await deliver({
      type: 'error',
      run_id: null,
      code: 'too-many-watches',
      message: 'a connection has at most 16 watches',
      detail: { id: 1 },
    });
    expect(client.variables.watchProblem.get()).toEqual({
      code: 'too-many-watches',
      message: 'a connection has at most 16 watches',
    });
    // Cleared once the subscriptions are sent again
    const other = client.variables.watch([SPEED], { periodMs: 20 });
    await vi.advanceTimersByTimeAsync(0);
    expect(client.variables.watchProblem.get()).toBeNull();
    slow.close();
    other.close();
  });

  it('does not call a run that ended before its first step one that waits to be resumed', async () => {
    const { client, http, deliver } = setup();
    const app = (await httpExamples()).find((e) => e.name === 'app')?.response;
    const loading = client.loadApp();
    http.expectOne('api/app').flush(app);
    await loading;
    const standing = {
      ...summary(1, ready()),
      phase: 'waiting',
      start_paused: true,
      time: null,
      result: null,
      launcher_state: holdingState,
    };
    await deliver({ ...hello(1), session: standing }, { type: 'synced', run_id: '1', last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(true);
    const dead = { ...standing, launcher: { ...standing.launcher, running: false, exit_code: 70 } };
    await deliver({ type: 'session', session: dead });
    expect(isHolding(client.session.session.get())).toBe(false);
  });

  it('forwards optional keepalive on write() without changing the request body', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()), { type: 'synced', run_id: '1', last_seq: null });

    const normal = client.variables.write({ Engine: { 'engine.demand': 10 } });
    const req1 = http.expectOne('api/write');
    expect(req1.request.keepalive).toBeUndefined();
    expect(req1.request.body).toEqual({
      run_id: '1',
      instance: 'x',
      writes: { Engine: { 'engine.demand': 10 } },
    });
    req1.flush({ run_id: '1', operation: 1 });
    await deliver(operation(1, 'applied', true));
    await expect(normal).resolves.toMatchObject({ seq: 1, state: 'applied' });

    const unloading = client.variables.write(
      { Engine: { 'engine.demand': 0 } },
      { keepalive: true },
    );
    const req2 = http.expectOne('api/write');
    expect(req2.request.keepalive).toBe(true);
    expect(req2.request.body).toEqual({
      run_id: '1',
      instance: 'x',
      writes: { Engine: { 'engine.demand': 0 } },
    });
    req2.flush({ run_id: '1', operation: 2 });
    await deliver(operation(2, 'applied', true));
    await expect(unloading).resolves.toMatchObject({ seq: 2, state: 'applied' });
  });
});

describe('the variables examples of the protocol', () => {
  let examples: Map<string, WebSocketExample>;
  beforeAll(async () => {
    examples = new Map((await webSocketExamples()).map((example) => [example.name, example]));
  });
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('follows a run with variables: the schema, the values while it stands still, the operation, the end', async () => {
    const { client, deliver } = setup();
    const watch = client.variables.watch([ENGINE, SPEED], { periodMs: 100, history: 10 });
    const seen: string[] = [];
    client.variables.state.subscribe((state) => seen.push(state.status));
    await deliver(...(examples.get('ws-variables')?.server ?? []));
    expect(seen.filter((s, i) => s !== seen[i - 1])).toEqual([
      'off',
      'unknown',
      'connecting',
      'ready',
      'ended',
    ]);
    expect(client.variables.state.get().peers.map((p) => p.name)).toEqual(['Engine', 'Dashboard']);
    expect(watch.values.get()).toEqual({
      Engine: {
        'engine.demand': { value: 40, sampleMs: 200, simMs: 200, paused: true },
        'engine.speed': { value: 600, sampleMs: 200, simMs: 200, paused: true },
      },
    });
    expect(watch.history(ENGINE)).toEqual([{ sampleMs: 200, value: 40 }]);
    expect(client.variables.operations.get()).toMatchObject([
      { seq: 1, state: 'applied', done: true },
    ]);
    expect(client.session.notices.get().map((n) => n.message)).toEqual([
      expect.stringContaining('period_ms must be'),
    ]);
  });

  it('shows why the variables cannot be read', async () => {
    const { client, deliver } = setup();
    await deliver(hello(1), ...(examples.get('ws-variables-failed')?.server ?? []));
    expect(client.variables.state.get()).toMatchObject({
      status: 'failed',
      reason: expect.stringContaining('cannot be reached'),
    });
  });
});

describe('four-stage write contract and virtual-time watchdog', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const multiRateCheck = {
    vehicle_sil_check: 1,
    file: 'simulation.yaml',
    result: 'passed',
    errors: 0,
    warnings: 0,
    notes: 0,
    stop_at_ms: null,
    participants: [
      {
        name: 'Engine',
        role: 'ecu',
        enabled: true,
        manual: false,
        step_ms: null,
        reported: { step_ns: 10_000_000 },
        variables: [],
      },
      {
        name: 'Dashboard',
        role: 'ecu',
        enabled: true,
        manual: false,
        step_ms: null,
        reported: { step_ns: 5_000_000 },
        variables: [],
      },
    ],
    networks: [],
    findings: [],
    scenario: null,
  };

  const multiPeersVariables = (engineConnected = true, dashConnected = true) => ({
    state: 'ready',
    reason: null,
    peers: [
      {
        name: 'Engine',
        connected: engineConnected,
        variables: [
          {
            name: 'engine.demand',
            type: 'real',
            role: 'input',
            writable: true,
            builtin: false,
            unit: '%',
            min: 0,
            max: 100,
            step: null,
            description: '',
            enum: null,
          },
        ],
      },
      {
        name: 'Dashboard',
        connected: dashConnected,
        variables: [
          {
            name: 'dash.brightness',
            type: 'real',
            role: 'input',
            writable: true,
            builtin: false,
            unit: '%',
            min: 0,
            max: 100,
            step: null,
            description: '',
            enum: null,
          },
        ],
      },
    ],
    peers_omitted: 0,
    variables_omitted: 0,
    operations: [],
  });

  const syncedRunHello = (
    id: string,
    launcherState: object,
    simMs: number | null = 0,
    vars: object = multiPeersVariables(),
  ) => [
    {
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'inst-1',
      session: {
        id,
        state: 'running',
        launcher: { pid: 10, running: true, exit_code: null, signal: null, silent_s: null },
        stop: 'none',
        phase: 'running',
        speed: 1,
        start_paused: false,
        check: multiRateCheck,
        time: simMs === null ? null : { sim_ms: simMs, actual_speed: 1 },
        launcher_state: launcherState,
        variables: vars,
      },
      replay: null,
    },
    { type: 'synced', run_id: id, last_seq: 1 },
  ];

  const makeState = (seq: number, fields: object) => ({
    seq,
    rev: seq,
    phase: 'running',
    paused: false,
    holding: false,
    paused_at_ms: null,
    speed: 1,
    pacing: 'on-time',
    stop: { requests: 0, by: null },
    scenario: false,
    allowed: { write: { ok: true, reason: null, message: null, caution: null } },
    outside: [],
    ...fields,
  });

  it('handles results arriving before applied() registration and never causes unhandled rejections when applied() is omitted', async () => {
    const { client, http, deliver } = setup();
    await deliver(...syncedRunHello('1', makeState(1, { paused: true, paused_at_ms: 0 }), 0));

    // 1) applied arrives before queued.applied() is registered
    const q1Promise = client.variables.enqueue({ Engine: { 'engine.demand': 55 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 11 });
    const q1 = await q1Promise;
    await deliver(operation(11, 'applied', true, { applied_at_ms: 10 }));
    await vi.advanceTimersByTimeAsync(60_000);
    const res1 = await q1.applied();
    expect(res1).toMatchObject({ seq: 11, state: 'applied' });

    // 2) terminal failure arrives after enqueue() but applied() is NEVER called -> zero unhandled rejection
    const q2Promise = client.variables.enqueue({ Engine: { 'engine.demand': 60 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 12 });
    await q2Promise;
    await deliver(operation(12, 'late', true, { reason: 'missed step' }));
    await vi.advanceTimersByTimeAsync(20_000);

    // 3) terminal failure already known when HTTP 200 arrives -> enqueue() rejects immediately with specific code
    const q3Promise = client.variables.enqueue({ Engine: { 'engine.demand': 65 } });
    await deliver(operation(13, 'invalid', true, { reason: 'out of range' }));
    http.expectOne('api/write').flush({ run_id: '1', operation: 13 });
    await expect(q3Promise).rejects.toMatchObject({
      kind: 'failed',
      code: 'invalid',
      operation: { seq: 13, state: 'invalid' },
    });

    // 4) 504 write-timeout with detail { operation, state: 'sent' } preserves partial operation metadata
    const q4Promise = client.variables.enqueue({ Engine: { 'engine.demand': 70 } });
    http.expectOne('api/write').flush(
      {
        error: {
          code: 'write-timeout',
          message: 'timed out waiting for queued',
          detail: { operation: 14, state: 'sent' },
        },
      },
      { status: 504, statusText: 'Gateway Timeout' },
    );
    await expect(q4Promise).rejects.toMatchObject({
      kind: 'no-answer',
      code: 'write-timeout',
      operation: { session: '1', seq: 14, state: 'sent', done: false },
    });
  });

  it('suspends across multi-rate pause/holding even when simMs >= requested atMs, then resolves or times out past due', async () => {
    const { client, http, deliver } = setup();
    await deliver(...syncedRunHello('1', makeState(1, { paused: true, paused_at_ms: 0 }), 0));

    // Multi-rate write at atMs: 5 -> Dashboard (5 ms) due at 5 ms, Engine (10 ms) due at 10 ms -> overall due = 10 ms
    const qPromise = client.variables.enqueue(
      { Engine: { 'engine.demand': 50 }, Dashboard: { 'dash.brightness': 80 } },
      { atMs: 5 },
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    const queued = await qPromise;
    let settled: unknown = 'pending';
    const applied = queued.applied({ timeoutMs: 2_000 }).then(
      (o) => (settled = o),
      (e: unknown) => (settled = e),
    );

    // Advance to runUntil(5) and pause at 5 ms: Dashboard applied at 5 ms, Engine still queued for 10 ms!
    await deliver(
      {
        type: 'operation',
        run_id: '1',
        seq: 1,
        state: 'queued',
        done: false,
        peers: {
          Dashboard: { state: 'applied', applied_at_ms: 5, late: false, reason: null },
          Engine: { state: 'queued', applied_at_ms: null, late: false, reason: null },
        },
      },
      {
        type: 'event',
        run_id: '1',
        event: { event: 'time', seq: 2, t_ms: 5, sim_ms: 5, actual_speed: 1 },
      },
      {
        type: 'event',
        run_id: '1',
        event: { event: 'state', t_ms: 5, ...makeState(3, { paused: true, paused_at_ms: 5 }) },
      },
    );

    // Paused at 5 ms while healthy and Engine connected: watchdog MUST stay suspended indefinitely!
    await vi.advanceTimersByTimeAsync(120_000);
    expect(settled).toBe('pending');

    // Also verify pause AT due (10 ms) stays suspended while healthy & connected (§2.1 F1 rule 2 > rule 3)
    await deliver(
      {
        type: 'event',
        run_id: '1',
        event: { event: 'time', seq: 4, t_ms: 10, sim_ms: 10, actual_speed: 1 },
      },
      {
        type: 'event',
        run_id: '1',
        event: { event: 'state', t_ms: 10, ...makeState(5, { paused: true, paused_at_ms: 10 }) },
      },
    );
    await vi.advanceTimersByTimeAsync(30_000);
    expect(settled).toBe('pending');

    // Resume at 10 ms: step [10, 20) is in liveness until sim_ms passes duePass = 20 ms, then enters armed-due and
    // never resets on further simMs progress!
    await deliver({
      type: 'event',
      run_id: '1',
      event: { event: 'state', t_ms: 11, ...makeState(6, { paused: false, paused_at_ms: null }) },
    });
    await vi.advanceTimersByTimeAsync(500);
    expect(settled).toBe('pending');
    await deliver({
      type: 'event',
      run_id: '1',
      event: { event: 'time', seq: 7, t_ms: 20, sim_ms: 20, actual_speed: 1 },
    });
    await vi.advanceTimersByTimeAsync(1_200);
    await deliver({
      type: 'event',
      run_id: '1',
      event: { event: 'time', seq: 8, t_ms: 30, sim_ms: 30, actual_speed: 1 },
    });
    expect(settled).toBe('pending');
    await vi.advanceTimersByTimeAsync(800);
    await applied;
    expect(settled).toBeInstanceOf(WriteError);
    expect(settled).toMatchObject({ kind: 'no-answer' });
  });

  it('scales liveness and armed-due with targetStepMs/speed without inflating timeoutMs/speed, and handles late:apply and coarse time_step_ms', async () => {
    const { client, http, deliver } = setup();
    const slowCheck = {
      ...multiRateCheck,
      participants: [
        ...multiRateCheck.participants,
        {
          name: 'SlowECU',
          role: 'ecu',
          enabled: true,
          manual: false,
          step_ms: 50,
          variables: [],
        },
      ],
    };
    const slowVars = {
      ...multiPeersVariables(),
      peers: [
        ...multiPeersVariables().peers,
        {
          name: 'SlowECU',
          connected: true,
          variables: [
            {
              name: 'slow.in',
              type: 'real',
              role: 'input',
              writable: true,
              builtin: false,
              unit: '',
              min: null,
              max: null,
              step: null,
              description: '',
              enum: null,
            },
          ],
        },
      ],
    };
    await deliver(
      {
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'inst-1',
        session: {
          id: '1',
          state: 'running',
          launcher: { pid: 10, running: true, exit_code: null, signal: null, silent_s: null },
          stop: 'none',
          phase: 'running',
          speed: 0.02,
          start_paused: false,
          check: slowCheck,
          time: { sim_ms: 0, actual_speed: 0.02 },
          launcher_state: makeState(1, { paused: false, speed: 0.02 }),
          variables: slowVars,
        },
        replay: null,
      },
      { type: 'synced', run_id: '1', last_seq: 1 },
    );

    // 1) At 0.02x with Engine (step 10 ms), effectiveMs = max(10_000, ceil((10/0.02)*2)+2000 = 3_000) = 10_000 ms
    //    (NOT inflated to 500_000 ms via timeoutMs / speed!).
    const qDeadlock = client.variables.enqueue(
      { Engine: { 'engine.demand': 40 } },
      { atMs: 1000, timeoutMs: 10_000 },
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    const q1 = await qDeadlock;
    let state1: unknown = 'pending';
    const p1 = q1.applied().then(
      (o) => (state1 = o),
      (e: unknown) => (state1 = e),
    );
    await vi.advanceTimersByTimeAsync(9_999);
    expect(state1).toBe('pending');
    await vi.advanceTimersByTimeAsync(1);
    await p1;
    expect(state1).toMatchObject({ kind: 'no-answer' });

    // 2) At 0.001x multi-rate (Dashboard 5 ms + SlowECU 50 ms) with atMs: 50 and { atMs: 0, late: 'apply' } at sim_ms=10:
    //    effectiveMs = max(10_000, ceil((50/0.001)*2) + 2000) = 102_000 ms; duePass = 50 + 50 = 100 ms.
    await deliver(
      {
        type: 'event',
        run_id: '1',
        event: { event: 'time', seq: 2, t_ms: 10_000, sim_ms: 10, actual_speed: 0.001 },
      },
      {
        type: 'event',
        run_id: '1',
        event: { event: 'state', t_ms: 10_000, ...makeState(3, { paused: false, speed: 0.001 }) },
      },
    );
    const qMulti = client.variables.enqueue(
      { Dashboard: { 'dash.brightness': 50 }, SlowECU: { 'slow.in': 7 } },
      { atMs: 50, timeoutMs: 10_000 },
    );
    const qLateApply = client.variables.enqueue(
      { SlowECU: { 'slow.in': 9 } },
      { atMs: 0, late: 'apply', timeoutMs: 10_000 },
    );
    http.requests[0].flush({ run_id: '1', operation: 2 });
    http.requests[1].flush({ run_id: '1', operation: 3 });
    http.requests.splice(0, 2);
    const [qm, ql] = await Promise.all([qMulti, qLateApply]);
    let outMulti: unknown = 'pending';
    let outLate: unknown = 'pending';
    const pm = qm.applied().then(
      (o) => (outMulti = o),
      (e: unknown) => (outMulti = e),
    );
    const pl = ql.applied().then(
      (o) => (outLate = o),
      (e: unknown) => (outLate = e),
    );

    // Advance 10 ms ticks up to sim_ms = 55 ms (45 s wall time): Dashboard applied at 50 ms, SlowECU still running [50, 100)!
    for (const sim of [20, 30, 40, 50, 55]) {
      await vi.advanceTimersByTimeAsync(sim === 55 ? 5_000 : 10_000);
      await deliver({
        type: 'event',
        run_id: '1',
        event: { event: 'time', seq: sim, t_ms: sim * 1000, sim_ms: sim, actual_speed: 0.001 },
      });
      if (sim === 50) {
        await deliver({
          type: 'operation',
          run_id: '1',
          seq: 2,
          state: 'queued',
          done: false,
          peers: {
            Dashboard: { state: 'applied', applied_at_ms: 50, late: false, reason: null },
            SlowECU: { state: 'queued', applied_at_ms: null, late: false, reason: null },
          },
        });
      }
    }
    // Wait another 40 s inside [50, 100) before SlowECU's step_end at 100 ms -> MUST NOT false-timeout!
    await vi.advanceTimersByTimeAsync(40_000);
    expect(outMulti).toBe('pending');
    expect(outLate).toBe('pending');

    await deliver(
      {
        type: 'operation',
        run_id: '1',
        seq: 2,
        state: 'applied',
        done: true,
        peers: {
          Dashboard: { state: 'applied', applied_at_ms: 50, late: false, reason: null },
          SlowECU: { state: 'applied', applied_at_ms: 50, late: false, reason: null },
        },
      },
      {
        type: 'operation',
        run_id: '1',
        seq: 3,
        state: 'applied',
        done: true,
        peers: {
          SlowECU: { state: 'applied', applied_at_ms: 50, late: true, reason: null },
        },
      },
    );
    await Promise.all([pm, pl]);
    expect(outMulti).toMatchObject({ seq: 2, state: 'applied' });
    expect(outLate).toMatchObject({ seq: 3, state: 'applied' });

    // 3) Mid-flight speed switch in armed-due preserves fractional dueUsed without resetting on simMs progress.
    const qDue = client.variables.enqueue(
      { SlowECU: { 'slow.in': 11 } },
      { atMs: 50, timeoutMs: 2_000 },
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 4 });
    const q4 = await qDue;
    let outDue: unknown = 'pending';
    const p4 = q4.applied().then(
      (o) => (outDue = o),
      (e: unknown) => (outDue = e),
    );
    await deliver({
      type: 'event',
      run_id: '1',
      event: { event: 'time', seq: 110, t_ms: 160_000, sim_ms: 160, actual_speed: 0.001 },
    });
    // At 0.001x, armed-due effectiveMs = 102_000 ms; consume 51_000 ms (50% of budget, including deliver's BATCH_MS),
    // then switch speed 0.001 -> 1!
    await vi.advanceTimersByTimeAsync(51_000 - BATCH_MS);
    expect(outDue).toBe('pending');
    await deliver(
      {
        type: 'event',
        run_id: '1',
        event: { event: 'state', t_ms: 211_000, ...makeState(115, { paused: false, speed: 1 }) },
      },
      {
        type: 'event',
        run_id: '1',
        event: { event: 'time', seq: 120, t_ms: 211_000, sim_ms: 170, actual_speed: 1 },
      },
    );
    // Remaining 50% of 2_000 ms = 1_000 ms (and simMs progress 110 -> 120 did NOT reset dueUsed!).
    await vi.advanceTimersByTimeAsync(999);
    expect(outDue).toBe('pending');
    await vi.advanceTimersByTimeAsync(1);
    await p4;
    expect(outDue).toMatchObject({ kind: 'no-answer' });

    // 4) Coarse controller time_step_ms (5000 ms) > participant.step_ms (10 ms) uses targetStepMs = 5000 ms.
    await deliver(
      {
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'inst-1',
        session: {
          id: '2',
          state: 'running',
          launcher: { pid: 11, running: true, exit_code: null, signal: null, silent_s: null },
          stop: 'none',
          phase: 'running',
          speed: 0.5,
          start_paused: false,
          check: {
            ...multiRateCheck,
            time_step_ms: 5000,
            participants: [
              {
                name: 'Engine',
                role: 'ecu',
                enabled: true,
                manual: false,
                step_ms: 10,
                variables: [],
              },
            ],
          },
          time: { sim_ms: 0, actual_speed: 0.5 },
          launcher_state: makeState(1, { paused: false, speed: 0.5 }),
          variables: multiPeersVariables(),
        },
        replay: null,
      },
      { type: 'synced', run_id: '2', last_seq: 1 },
    );
    // At speed 0.5 and targetStepMs = 5000, effectiveMs = max(2_000, ceil((5000/0.5)*2) + 2000) = 22_000 ms.
    const qCoarse = client.variables.enqueue(
      { Engine: { 'engine.demand': 15 } },
      { timeoutMs: 2_000 },
    );
    http.expectOne('api/write').flush({ run_id: '2', operation: 1 });
    const qc = await qCoarse;
    let outCoarse: unknown = 'pending';
    const pc = qc.applied().then(
      (o) => (outCoarse = o),
      (e: unknown) => (outCoarse = e),
    );
    await vi.advanceTimersByTimeAsync(9_500);
    expect(outCoarse).toBe('pending');
    await deliver(operation(1, 'applied', true, { applied_at_ms: 5000 }, 2));
    await pc;
    expect(outCoarse).toMatchObject({ seq: 1, state: 'applied' });
  });

  it('bounds armed-unhealthy across ECU disconnect while paused, flapping health, and stuck non-sync', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(...syncedRunHello('1', makeState(1, { paused: true, paused_at_ms: 0 }), 0));

    // 1) ECU disconnect while paused -> exits suspended into armed-unhealthy and rejects within timeoutMs
    const q1Promise = client.variables.enqueue({ Engine: { 'engine.demand': 10 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    const q1 = await q1Promise;
    let out1: unknown = 'pending';
    const p1 = q1.applied({ timeoutMs: 3_000 }).then(
      (o) => (out1 = o),
      (e: unknown) => (out1 = e),
    );
    await vi.advanceTimersByTimeAsync(20_000); // suspended while healthy & paused
    expect(out1).toBe('pending');

    await deliver({
      type: 'variables',
      run_id: '1',
      variables: multiPeersVariables(false, true),
    });
    await vi.advanceTimersByTimeAsync(2_999);
    expect(out1).toBe('pending');
    await vi.advanceTimersByTimeAsync(1);
    await p1;
    expect(out1).toMatchObject({ kind: 'no-answer' });

    // Reconnect Engine and test flapping health (4s unhealthy -> 30s healthy paused -> 4s unhealthy -> 30s healthy -> 2s unhealthy)
    await deliver({
      type: 'variables',
      run_id: '1',
      variables: multiPeersVariables(true, true),
    });
    const q2Promise = client.variables.enqueue({ Engine: { 'engine.demand': 20 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 2 });
    const q2 = await q2Promise;
    let out2: unknown = 'pending';
    const p2 = q2.applied({ timeoutMs: 10_000 }).then(
      (o) => (out2 = o),
      (e: unknown) => (out2 = e),
    );

    // Flap 1: 4_000 ms unhealthy via resync (without synced)
    await deliver({ type: 'resync' });
    await vi.advanceTimersByTimeAsync(4_000 - BATCH_MS);
    expect(out2).toBe('pending');
    await deliver(...syncedRunHello('1', makeState(2, { paused: true, paused_at_ms: 0 }), 0));
    await vi.advanceTimersByTimeAsync(30_000); // suspended while healthy
    expect(out2).toBe('pending');

    // Flap 2: another 4_000 ms unhealthy via socket drop (cumulative ~8_000 ms)
    socket().fail();
    await vi.advanceTimersByTimeAsync(4_000);
    expect(out2).toBe('pending');
    socket().open();
    await deliver(...syncedRunHello('1', makeState(3, { paused: true, paused_at_ms: 0 }), 0));
    await vi.advanceTimersByTimeAsync(30_000);
    expect(out2).toBe('pending');

    // Flap 3: remaining ~2_000 ms unhealthy -> MUST reject instead of resetting to 10_000 ms!
    await deliver({ type: 'resync' });
    await vi.advanceTimersByTimeAsync(2_100);
    await p2;
    expect(out2).toMatchObject({ kind: 'no-answer' });
  });

  it('supports AbortSignal before dispatch, during HTTP dispatch, and during applied()', async () => {
    const { client, http, deliver } = setup();
    await deliver(...syncedRunHello('1', makeState(1, { paused: true, paused_at_ms: 0 }), 0));

    // Pre-aborted before enqueue
    const pre = new AbortController();
    pre.abort();
    await expect(
      client.variables.enqueue({ Engine: { 'engine.demand': 1 } }, { signal: pre.signal }),
    ).rejects.toMatchObject({ kind: 'refused', code: 'aborted' });
    http.expectNone('api/write');

    // Aborted while HTTP POST api/write is in flight
    const midHttp = new AbortController();
    const enqMid = client.variables.enqueue(
      { Engine: { 'engine.demand': 2 } },
      { signal: midHttp.signal },
    );
    const reqMid = http.expectOne('api/write');
    midHttp.abort();
    reqMid.fail(new HttpError(0, 'aborted', null));
    await expect(enqMid).rejects.toMatchObject({ kind: 'no-answer', code: 'aborted' });

    // Aborted while waiting on queued.applied()
    const midWait = new AbortController();
    const qPromise = client.variables.enqueue({ Engine: { 'engine.demand': 3 } });
    http.expectOne('api/write').flush({ run_id: '1', operation: 9 });
    const queued = await qPromise;
    await deliver(operation(9, 'queued', false));
    const applied = queued.applied({ signal: midWait.signal });
    midWait.abort();
    await expect(applied).rejects.toMatchObject({
      kind: 'no-answer',
      code: 'aborted',
      operation: { seq: 9, state: 'queued' },
    });

    // Explicit queued.applied() with no signal MUST NOT inherit enqueue()'s signal!
    const enqOnly = new AbortController();
    const qNoInheritPromise = client.variables.enqueue(
      { Engine: { 'engine.demand': 4 } },
      { signal: enqOnly.signal },
    );
    http.expectOne('api/write').flush({ run_id: '1', operation: 10 });
    const qNoInherit = await qNoInheritPromise;
    enqOnly.abort();
    let plainApplied: unknown = 'pending';
    const plainWait = qNoInherit.applied().then(
      (o) => (plainApplied = o),
      (e: unknown) => (plainApplied = e),
    );
    await vi.advanceTimersByTimeAsync(5_000);
    expect(plainApplied).toBe('pending');
    await deliver(operation(10, 'applied', true, { applied_at_ms: 10 }));
    await plainWait;
    expect(plainApplied).toMatchObject({ seq: 10, state: 'applied' });
  });

  it('exits suspended during in-flight step()/runUntil() (onItsWay or paused_at_ms: null) so hung ECUs time out', async () => {
    const { client, http, deliver } = setup();
    await deliver(
      ...syncedRunHello(
        '1',
        makeState(1, { phase: 'waiting', paused: true, holding: true, paused_at_ms: null }),
        null,
      ),
    );

    // 1) step(10) accepted while holding: command arrives before state update -> onItsWay === true -> liveness catches hang!
    const q1p = client.variables.enqueue({ Engine: { 'engine.demand': 42 } }, { timeoutMs: 5_000 });
    http.expectOne('api/write').flush({ run_id: '1', operation: 1 });
    const q1 = await q1p;
    let out1: unknown = 'pending';
    const p1 = q1.applied().then(
      (o) => (out1 = o),
      (e: unknown) => (out1 = e),
    );
    await vi.advanceTimersByTimeAsync(60_000); // suspended while holding before step
    expect(out1).toBe('pending');

    await deliver({
      type: 'event',
      run_id: '1',
      event: {
        event: 'command',
        seq: 2,
        t_ms: 10,
        command: 'step',
        id: 1,
        accepted: true,
        ms: 10,
        at_ms: 10,
      },
    });
    await vi.advanceTimersByTimeAsync(5_000 - BATCH_MS - 1);
    expect(out1).toBe('pending');
    await vi.advanceTimersByTimeAsync(1);
    await p1;
    expect(out1).toMatchObject({ kind: 'no-answer' });

    // 2) state { holding: false, paused: true, paused_at_ms: null } (released holding, not yet standing) -> liveness catches hang!
    await deliver(
      ...syncedRunHello(
        '2',
        makeState(1, { phase: 'running', paused: true, holding: false, paused_at_ms: null }),
        0,
      ),
    );
    const q2p = client.variables.enqueue({ Engine: { 'engine.demand': 43 } }, { timeoutMs: 4_000 });
    http.expectOne('api/write').flush({ run_id: '2', operation: 1 });
    const q2 = await q2p;
    let out2: unknown = 'pending';
    const p2 = q2.applied().then(
      (o) => (out2 = o),
      (e: unknown) => (out2 = e),
    );
    await vi.advanceTimersByTimeAsync(4_000);
    await p2;
    expect(out2).toMatchObject({ kind: 'no-answer' });
  });

  it('validates atMs, late, keepalive, signal, sub-ms timeoutMs, and preserves multi-ECU settled peers across OPERATIONS_KEPT eviction', async () => {
    const { client, http, deliver } = setup();
    await deliver(...syncedRunHello('1', makeState(1, { paused: true, paused_at_ms: 0 }), 0));

    for (const badOpts of [
      { atMs: Number.NaN },
      { atMs: Number.POSITIVE_INFINITY },
      { atMs: Number.NEGATIVE_INFINITY },
      { atMs: -1 },
      { atMs: '10' as unknown as number },
      { late: 'never' as unknown as 'reject' },
      { late: null as unknown as 'reject' },
      { keepalive: 1 as unknown as boolean },
      { signal: {} as unknown as AbortSignal },
      { signal: { aborted: false } as unknown as AbortSignal },
    ]) {
      await expect(
        client.variables.enqueue({ Engine: { 'engine.demand': 1 } }, badOpts),
      ).rejects.toMatchObject({ kind: 'refused', code: 'invalid-argument' });
    }
    http.expectNone('api/write');

    // Multi-ECU write: Dashboard settles early ('applied' in op 1, 'late' in op 2), then disconnects while paused,
    // and 64 other operations evict ops 1 & 2 from operationsValue -> neither falsely enters armed-unhealthy!
    const qAppliedPeer = client.variables.enqueue({
      Engine: { 'engine.demand': 10 },
      Dashboard: { 'dash.brightness': 20 },
    });
    const qLatePeer = client.variables.enqueue({
      Engine: { 'engine.demand': 15 },
      Dashboard: { 'dash.brightness': 25 },
    });
    http.requests[0].flush({ run_id: '1', operation: 1 });
    http.requests[1].flush({ run_id: '1', operation: 2 });
    http.requests.splice(0, 2);
    const [qa, ql] = await Promise.all([qAppliedPeer, qLatePeer]);

    await deliver(
      {
        type: 'operation',
        run_id: '1',
        seq: 1,
        state: 'queued',
        done: false,
        peers: {
          Dashboard: { state: 'applied', applied_at_ms: 0, late: false, reason: null },
          Engine: { state: 'queued', applied_at_ms: null, late: false, reason: null },
        },
      },
      {
        type: 'operation',
        run_id: '1',
        seq: 2,
        state: 'queued',
        done: false,
        peers: {
          Dashboard: { state: 'late', applied_at_ms: null, late: false, reason: 'missed' },
          Engine: { state: 'queued', applied_at_ms: null, late: false, reason: null },
        },
      },
    );

    let stateA: unknown = 'pending';
    let stateL: unknown = 'pending';
    const pa = qa.applied({ timeoutMs: 3_000 }).then(
      (o) => (stateA = o),
      (e: unknown) => (stateA = e),
    );
    const pl = ql.applied({ timeoutMs: 3_000 }).then(
      (o) => (stateL = o),
      (e: unknown) => (stateL = e),
    );

    // Evict seq 1 & 2 with 70 completed operations, then disconnect already-settled Dashboard while Engine stays paused!
    const flood = Array.from({ length: OPERATIONS_KEPT + 6 }, (_, i) =>
      operation(100 + i, 'applied', true),
    );
    await deliver(...flood, {
      type: 'variables',
      run_id: '1',
      variables: multiPeersVariables(true, false),
    });
    expect(client.variables.operations.get().some((o) => o.seq <= 2)).toBe(false);

    await vi.advanceTimersByTimeAsync(30_000);
    expect(stateA).toBe('pending');
    expect(stateL).toBe('pending');

    // Also validate waitOptions signal & sub-ms timeoutMs > 0 (0.5 ms arms 1 ms timer rather than synchronous 0 ms reject)
    await expect(qa.applied({ signal: {} as unknown as AbortSignal })).rejects.toMatchObject({
      kind: 'refused',
      code: 'invalid-argument',
    });

    await deliver({
      type: 'event',
      run_id: '1',
      event: { event: 'state', t_ms: 50, ...makeState(10, { paused: false, paused_at_ms: null }) },
    });
    let subMsSettled = false;
    const subMsPromise = qa.applied({ timeoutMs: 0.5 }).catch((e: unknown) => {
      subMsSettled = true;
      return e;
    });
    await Promise.resolve();
    expect(subMsSettled).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    const subMsErr = await subMsPromise;
    expect(subMsErr).toMatchObject({
      kind: 'no-answer',
      operation: { seq: 1, state: 'queued' },
    });

    // Settle ops 1 & 2 deterministically
    await deliver(
      {
        type: 'operation',
        run_id: '1',
        seq: 1,
        state: 'applied',
        done: true,
        peers: {
          Dashboard: { state: 'applied', applied_at_ms: 0, late: false, reason: null },
          Engine: { state: 'applied', applied_at_ms: 10, late: false, reason: null },
        },
      },
      {
        type: 'operation',
        run_id: '1',
        seq: 2,
        state: 'mixed',
        done: true,
        peers: {
          Dashboard: { state: 'late', applied_at_ms: null, late: false, reason: 'missed' },
          Engine: { state: 'applied', applied_at_ms: 10, late: false, reason: null },
        },
      },
    );
    await Promise.all([pa, pl]);
    expect(stateA).toMatchObject({ seq: 1, state: 'applied' });
    expect(stateL).toMatchObject({ kind: 'failed', code: 'mixed' });
  });
});
