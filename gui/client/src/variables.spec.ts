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

const summary = (id: number, variables?: unknown) => ({
  id,
  state: 'running',
  launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
  stop: 'none',
  ...(variables === undefined ? {} : { variables }),
});
const hello = (id: number | null, variables?: unknown) => ({
  type: 'hello',
  vehicle_sil_gui: 1,
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
  session = 1,
) => ({
  type: 'values',
  id,
  session,
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
const operation = (seq: number, state: string, done: boolean, peer: object = {}, session = 1) => ({
  type: 'operation',
  session,
  seq,
  state,
  done,
  peers: { Engine: { state, applied_at_ms: null, late: false, reason: null, ...peer } },
});

describe('readOperation', () => {
  it('says which writes took effect, and null for what is absent or not a position', () => {
    const peers = (p: object) =>
      readOperation(operation(3, 'rejected', true, p), 1)?.peers['Engine'];
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
      session: 1,
      status: 'connecting',
      peers: [{}],
    });
    await deliver({ type: 'variables', session: 1, variables: ready() });
    const state = client.variables.state.get();
    expect(state.status).toBe('ready');
    expect(state.peers[0].variables[0]).toEqual({
      name: 'engine.demand',
      type: 'real',
      role: 'input',
      writable: true,
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
      session: 2,
      status: 'unknown',
      peers: [],
    });
    await deliver({
      type: 'variables',
      session: 2,
      variables: { state: 'off', reason: 'no control server' },
    });
    expect(client.variables.state.get()).toMatchObject({
      session: 2,
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
      4,
    );
    expect(state).toEqual({
      session: 4,
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
      { type: 'variables', session: 2, variables: ready() },
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

  it('resolves a write when the operation is applied, whichever comes first, the answer or the result', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const first = client.variables.write(
      { Engine: { 'engine.demand': 40 } },
      { atMs: 100, late: 'apply' },
    );
    const request = http.expectOne('api/write');
    expect(request.request.body).toEqual({
      session: 1,
      writes: { Engine: { 'engine.demand': 40 } },
      at_ms: 100,
      late: 'apply',
    });
    request.flush({ session_id: 1, operation: 7 });
    await deliver(operation(7, 'sent', false), operation(7, 'queued', false));
    expect(client.variables.operations.get().map((o) => o.state)).toEqual(['queued']);
    await deliver(
      operation(7, 'applied', true, { applied_at_ms: 100, late: true, applied_writes: 1 }),
    );
    await expect(first).resolves.toEqual({
      session: 1,
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
    http.expectOne('api/write').flush({ session_id: 1, operation: 8 });
    await expect(second).resolves.toMatchObject({ seq: 8, state: 'applied' });
    // A settled operation is not set back by an old state arriving late
    await deliver(operation(8, 'queued', false));
    expect(client.variables.operations.get().map((o) => [o.seq, o.state])).toEqual([
      [7, 'applied'],
      [8, 'applied'],
    ]);
    http.verify();
  });

  it('tells when the server took a write, before its result, and not when it refused it', async () => {
    const { client, http, deliver, errors } = setup();
    await deliver(hello(1, ready()));
    const accepted: unknown[] = [];
    const taken = client.variables.write(
      { Engine: { 'engine.demand': 40 } },
      { onAccepted: (operation) => accepted.push(operation) },
    );
    http.expectOne('api/write').flush({ session_id: 1, operation: 7 });
    expect(accepted).toEqual([{ session: 1, seq: 7 }]);
    await deliver(operation(7, 'applied', true, { applied_at_ms: 100 }));
    await expect(taken).resolves.toMatchObject({ seq: 7 });
    // Not called for a refused write. Even if the callback throws, the write goes ahead
    const refused = client.variables.write(
      { Engine: { 'engine.demand': 400 } },
      { onAccepted: (operation) => accepted.push(operation) },
    );
    http
      .expectOne('api/write')
      .flush(
        { error: { code: 'invalid-field', message: 'out of range', detail: null } },
        { status: 400, statusText: 'Bad Request' },
      );
    await expect(refused).rejects.toMatchObject({ kind: 'refused' });
    const throwing = client.variables.write(
      { Engine: { 'engine.demand': 41 } },
      {
        onAccepted: () => {
          throw new Error('boom');
        },
      },
    );
    http.expectOne('api/write').flush({ session_id: 1, operation: 8 });
    await deliver(operation(8, 'applied', true, { applied_at_ms: 110 }));
    await expect(throwing).resolves.toMatchObject({ seq: 8 });
    expect(accepted).toEqual([{ session: 1, seq: 7 }]);
    expect(errors).toHaveLength(1);
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
    http.expectOne('api/write').flush({ session_id: 1, operation: 3 });
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
    http.expectOne('api/write').flush({ session_id: 1, operation: 4 });
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
    http.expectOne('api/write').flush({ session_id: 1, operation: 5 });
    const replaced = old.catch((e: unknown) => e);
    await deliver({ type: 'session', session: summary(2) });
    expect(await replaced).toMatchObject({ kind: 'no-answer' });
    const open = client.variables.write({ Engine: { 'engine.demand': 5 } });
    http.expectOne('api/write').flush({ session_id: 2, operation: 6 });
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
    http.expectOne('api/write').flush({ session_id: 1, operation: 9 });
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
    const accepted: unknown[] = [];
    const write = client.variables
      .write({ Engine: { 'engine.demand': 2 } }, { onAccepted: (o) => accepted.push(o) })
      .catch((e: unknown) => e);
    const request = http.expectOne('api/write');
    expect(request.request.body).toMatchObject({ session: 1 });
    // The next run started before the answer
    await deliver({ type: 'session', session: summary(2) });
    expect(await write).toMatchObject({ kind: 'no-answer' });
    // A late answer for the previous run: not counted as accepted
    request.flush({ session_id: 1, operation: 3 });
    await vi.advanceTimersByTimeAsync(0);
    expect(accepted).toEqual([]);
    // The next write is aimed at the new run
    const next = client.variables.write({ Engine: { 'engine.demand': 3 } }).catch(() => undefined);
    expect(http.expectOne('api/write').request.body).toMatchObject({ session: 2 });
    client.close();
    await next;
  });

  it('gives up a write whose result left the summary while the connection was down, not a newer one', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    const lost = client.variables
      .write({ Engine: { 'engine.demand': 40 } })
      .catch((e: unknown) => e);
    http.expectOne('api/write').flush({ session_id: 1, operation: 3 });
    await deliver(operation(3, 'queued', false));
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    // A write sent before the reconnect has a higher number than the operations in the summary
    const newer = client.variables.write({ Engine: { 'engine.demand': 41 } });
    http.expectOne('api/write').flush({ session_id: 1, operation: 80 });
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
    const write = client.variables.write(
      { Engine: { 'engine.demand': 40 } },
      { timeoutMs: Infinity },
    );
    write.then(
      (o) => (outcome = o),
      (e: unknown) => (outcome = e),
    );
    http.expectOne('api/write').flush({ session_id: 1, operation: 1 });
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
    // The same with the api/session summary (fetchSession)
    client.fetchSession().catch(() => undefined);
    http.expectOne('api/session').flush({ session: summary(1, again) });
    await vi.advanceTimersByTimeAsync(0);
    expect(outcome).toBe('waiting');
    await deliver(operation(1, 'applied', true));
    await expect(write).resolves.toMatchObject({ seq: 1, state: 'applied' });
  });

  it('does not guess from a summary without the boundary, and takes a result out of a long summary', async () => {
    const { client, http, socket, deliver } = setup();
    await deliver(hello(1, ready()));
    let first: unknown = 'waiting';
    client.variables.write({ Engine: { 'engine.demand': 1 } }, { timeoutMs: Infinity }).then(
      (o) => (first = o),
      (e: unknown) => (first = e),
    );
    http.expectOne('api/write').flush({ session_id: 1, operation: 2 });
    const second = client.variables.write(
      { Engine: { 'engine.demand': 2 } },
      { timeoutMs: Infinity },
    );
    http.expectOne('api/write').flush({ session_id: 1, operation: 3 });
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
      .write({ Engine: { 'engine.demand': 40 } }, { timeoutMs: Infinity })
      .catch((e: unknown) => e);
    http.expectOne('api/write').flush({ session_id: 1, operation: 1 });
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
        .write(
          { Engine: { 'engine.demand': value } },
          { timeoutMs: Infinity, onAccepted: (o) => accepted.push(o.seq) },
        )
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
    requests[0].flush({ session_id: 1, operation: 3 });
    requests[1].flush({ session_id: 1, operation: 4 });
    requests[2].flush({ session_id: 1, operation: 5 });
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
      { type: 'operation', session: 1 },
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
    await deliver(hello(1), { type: 'synced', session: 1, last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(false);
    await deliver({ ...hello(1), session: standing }, { type: 'synced', session: 1, last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(true);
    // The first step has begun (a resume, a step): the launcher's next state says so
    await deliver({
      type: 'event',
      session: 1,
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

  it('waits as long as asked for a result: for ever, the longest a timer takes, or the default', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const outcomes: Record<string, string> = {};
    let seq = 100;
    const start = (name: string, timeoutMs: number) => {
      const write = client.variables.write({ Engine: { 'engine.demand': 1 } }, { timeoutMs });
      write.then(
        () => (outcomes[name] = 'applied'),
        (e: WriteError) => (outcomes[name] = e.kind),
      );
      http.expectOne('api/write').flush({ session_id: 1, operation: ++seq });
      return seq;
    };
    const forever = start('forever', Infinity);
    const huge = start('huge', 2 ** 40);
    start('nan', NaN);
    start('negative', -5);
    start('zero', 0);
    await vi.advanceTimersByTimeAsync(9_999);
    expect(outcomes).toEqual({}); // Neither large values nor non-numbers time out at once
    await vi.advanceTimersByTimeAsync(1);
    expect(outcomes).toEqual({ nan: 'no-answer', negative: 'no-answer', zero: 'no-answer' });
    await vi.advanceTimersByTimeAsync(24 * 3600 * 1000);
    expect(Object.keys(outcomes).length).toBe(3);
    await deliver(operation(huge, 'applied', true), operation(forever, 'applied', true));
    expect(outcomes).toMatchObject({ forever: 'applied', huge: 'applied' });
    // A write that never times out is still cleared by close(). So is a write waiting for the HTTP answer
    start('open', Infinity);
    const flying = client.variables
      .write({ Engine: { 'engine.demand': 2 } })
      .catch((e: unknown) => e);
    client.close();
    await vi.advanceTimersByTimeAsync(0);
    expect(outcomes['open']).toBe('closed');
    expect(await flying).toMatchObject({ kind: 'closed' });
    http.expectOne('api/write').flush({ session_id: 1, operation: 999 }); // A late answer is no longer used
    expect(vi.getTimerCount()).toBe(0);
  });

  it('applies a result that came in the same batch as the next run before it gives the write up', async () => {
    const { client, http, deliver } = setup();
    await deliver(hello(1, ready()));
    const write = client.variables.write({ Engine: { 'engine.demand': 1 } });
    http.expectOne('api/write').flush({ session_id: 1, operation: 5 });
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
      { type: 'variables', session: 2, variables: ready() },
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
      session: null,
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
    await deliver({ ...hello(1), session: standing }, { type: 'synced', session: 1, last_seq: 0 });
    expect(isHolding(client.session.session.get())).toBe(true);
    const dead = { ...standing, launcher: { ...standing.launcher, running: false, exit_code: 70 } };
    await deliver({ type: 'session', session: dead });
    expect(isHolding(client.session.session.get())).toBe(false);
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
