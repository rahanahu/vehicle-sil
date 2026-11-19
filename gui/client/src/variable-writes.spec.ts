import { createClient } from './client';
import { BATCH_MS } from './connection';
import { FakeHttp, FakeRequest } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { BoundVariable, VariableBinding } from './variable-binding';
import { VariableRef } from './variables';

// Checks the write status transitions (writeStatus and pending of client.variables.bind) by building orderings: sent →
// received → settled, with an input refused before sending (reject), a next value, a change of run or a reconnect in
// between, no "value sent" is left behind and the next write is sent. Server notifications come from a fake WebSocket,
// and api/write is answered by fake HTTP.

const OFFSET: VariableRef = { peer: 'Engine', name: 'engine.offset' };

const variables = (state: string, more: object = {}) => ({
  state,
  reason: null,
  peers: [
    {
      name: 'Engine',
      connected: true,
      variables: [
        {
          name: 'engine.offset',
          type: 'real',
          role: 'input',
          writable: true,
          unit: 'rpm',
          min: null,
          max: null,
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
  ...more,
});

/** The launcher's state of a running run that takes writes (until a `state` says so, bind does not write). */
const TAKES_WRITES = {
  seq: 5,
  rev: 5,
  phase: 'running',
  paused: false,
  holding: false,
  speed: 1,
  pacing: 'on-time',
  stop: { requests: 0, by: null },
  scenario: false,
  allowed: { write: { ok: true, reason: null, message: null, caution: null } },
  outside: [],
};

const hello = (id = 1, more: object = {}) => ({
  type: 'hello',
  vehicle_sil_gui: 2,
  version: '0.1.0',
  instance: 'x',
  session: {
    id,
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: variables('ready', more),
    launcher_state: TAKES_WRITES,
  },
  replay: null,
});

const peers = (state: string) => ({
  Engine: { state, applied_at_ms: state === 'applied' ? 200 : null, late: false, reason: null },
});
const operation = (seq: number, state: string, done: boolean) => ({
  type: 'operation',
  session: 1,
  seq,
  state,
  done,
  peers: peers(state),
});
/** One entry of operations in a summary. */
const listed = (seq: number, state: string, done: boolean) => ({
  seq,
  state,
  done,
  peers: peers(state),
});

const REFUSED = {
  error: { code: 'variables-not-ready', message: 'the variables are not read yet', detail: null },
};

describe('the writes of a bound variable', () => {
  let fake: FakeWebSockets;
  let http: FakeHttp;
  let bound: VariableBinding;
  /** The last status delivered to the subscriber (if none yet, the status when bound). */
  let told: BoundVariable;

  beforeEach(async () => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    http = new FakeHttp();
    const client = createClient({
      baseUrl: 'http://127.0.0.1:8765/',
      token: 'secret',
      socket: fake.factory,
      http: http.transport,
      onError: (error) => {
        throw error;
      },
    });
    client.connect();
    bound = client.variables.bind(OFFSET);
    told = bound.state.get();
    bound.state.subscribe((state) => (told = state));
    socket().open();
    await deliver(hello());
  });

  afterEach(() => {
    bound.close();
    vi.useRealTimers();
  });

  const socket = () => fake.sockets[fake.sockets.length - 1];

  async function deliver(...messages: unknown[]) {
    for (const message of messages) socket().send(message);
    await vi.advanceTimersByTimeAsync(BATCH_MS);
  }

  /** Runs the Promise continuations (the writes settling). */
  const turn = () => vi.advanceTimersByTimeAsync(0);

  /** Takes an api/write not answered yet. */
  const requests = (): FakeRequest[] => {
    const found = http.requests.filter((r) => r.request.path === 'api/write');
    for (const request of found) http.requests.splice(http.requests.indexOf(request), 1);
    return found;
  };
  const valueOf = (request: FakeRequest) =>
    (request.request.body as { writes: { Engine: Record<string, unknown> } }).writes.Engine[
      'engine.offset'
    ];

  /**
   * The current status. Before reading, checks that the subscriber got the same status: get() rebuilds on every read,
   * so a missed notification (only the subscribed screen stuck with a stale status) cannot be found by looking at
   * get() alone.
   */
  const now = () => {
    const before = told;
    const fresh = bound.state.get();
    expect(before, 'the subscribers were not told of the last change').toEqual(fresh);
    return fresh;
  };
  const status = () => now().writeStatus;
  const pending = () => now().pending;
  /** The test bodies keep their pre-move form (writes.write(ref, value)): goes through a bind of the same variable. */
  const writes = {
    write: (_ref: VariableRef, value: number) => bound.write(value),
    reject: (_ref: VariableRef, message: string) => bound.reject(message),
  };

  /** The next write is sent at once instead of held (not stuck behind the previous write). */
  function expectNextIsSent(value: number) {
    writes.write(OFFSET, value);
    const sent = requests();
    expect(sent.map(valueOf)).toEqual([value]);
    return sent[0];
  }

  it('goes from sending over taken to applied, and shows the value sent until then', async () => {
    writes.write(OFFSET, 50);
    expect(status()).toEqual({ kind: 'sending' });
    expect(pending()).toBe(50);
    requests()[0].flush({ session_id: 1, operation: 4 });
    expect(status()).toEqual({ kind: 'queued', seq: 4 });
    expect(pending()).toBe(50);
    await deliver(operation(4, 'applied', true));
    expect(status()).toEqual({ kind: 'applied', atMs: 200 });
    expect(pending()).toBeNull();
  });

  // Ordering A: an input refused before sending, while accepted and waiting for the result
  for (const [result, state] of [
    ['applied', 'applied'],
    ['failed', 'late'],
  ] as const) {
    it(`drops the value sent when a write taken before an input was refused is ${result}`, async () => {
      writes.write(OFFSET, 50);
      requests()[0].flush({ session_id: 1, operation: 4 });
      await deliver(operation(4, 'queued', false));
      writes.reject(OFFSET, 'not a number');
      // The input error does not touch the waiting write: the value sent is still shown
      expect(status()).toEqual({ kind: 'invalid', message: 'not a number' });
      expect(pending()).toBe(50);
      await deliver(operation(4, state, true));
      expect(pending()).toBeNull();
      // The status field stays at the last action (the refused input)
      expect(status()).toEqual({ kind: 'invalid', message: 'not a number' });
      const next = expectNextIsSent(60);
      expect(status()).toEqual({ kind: 'sending' });
      expect(pending()).toBe(60);
      next.flush({ session_id: 1, operation: 5 });
      await deliver(operation(5, 'applied', true));
      expect(status()).toEqual({ kind: 'applied', atMs: 200 });
      expect(pending()).toBeNull();
    });
  }

  // Ordering B: an input refused before sending while sending (before the server's answer), and then the write is
  // refused
  for (const [name, answer] of [
    ['refused (409)', { status: 409, statusText: 'Conflict' }],
    ['not answered in time (504)', { status: 504, statusText: 'Gateway Timeout' }],
    ['not answered at all', { status: 0, statusText: '' }],
  ] as const) {
    it(`goes on writing when a write sent before an input was refused is ${name}`, async () => {
      writes.write(OFFSET, 50);
      const [first] = requests();
      writes.reject(OFFSET, 'not a number');
      expect(pending()).toBe(50);
      if (answer.status === 0) first.flush(null, answer);
      else first.flush(REFUSED, answer);
      await turn();
      expect(pending()).toBeNull();
      expect(status()).toEqual({ kind: 'invalid', message: 'not a number' });
      const next = expectNextIsSent(60);
      next.flush({ session_id: 1, operation: 5 });
      expect(status()).toEqual({ kind: 'queued', seq: 5 });
    });
  }

  it('sends the last value kept back once the write before it was taken, also after a refused input', async () => {
    writes.write(OFFSET, 50);
    const [first] = requests();
    writes.write(OFFSET, 55);
    writes.write(OFFSET, 60);
    expect(pending()).toBe(60);
    writes.reject(OFFSET, 'not a number');
    expect(requests()).toHaveLength(0);
    first.flush({ session_id: 1, operation: 4 });
    const [second] = requests();
    expect(valueOf(second)).toBe(60);
    expect(status()).toEqual({ kind: 'sending' });
    // The result of the earlier write does not touch the later write's status or value sent
    await deliver(operation(4, 'applied', true));
    expect(status()).toEqual({ kind: 'sending' });
    expect(pending()).toBe(60);
    second.flush({ session_id: 1, operation: 5 });
    await deliver(operation(5, 'late', true));
    expect(status()).toMatchObject({ kind: 'failed' });
    expect(pending()).toBeNull();
  });

  it('does not send the value kept back when the write before it is refused, and says why', async () => {
    writes.write(OFFSET, 50);
    const [first] = requests();
    writes.write(OFFSET, 60);
    first.flush(REFUSED, { status: 409, statusText: 'Conflict' });
    await turn();
    expect(requests()).toHaveLength(0);
    expect(status()).toMatchObject({ kind: 'refused', code: 'variables-not-ready' });
    expect(pending()).toBeNull();
    expectNextIsSent(70);
  });

  it('shows the result of the older write only while no newer one was sent', async () => {
    writes.write(OFFSET, 50);
    requests()[0].flush({ session_id: 1, operation: 4 });
    writes.write(OFFSET, 60); // 4 has been received, so this is sent at once
    const [second] = requests();
    await deliver(operation(4, 'late', true));
    expect(status()).toEqual({ kind: 'sending' });
    expect(pending()).toBe(60);
    second.flush({ session_id: 1, operation: 5 });
    await deliver(operation(5, 'applied', true));
    expect(status()).toEqual({ kind: 'applied', atMs: 200 });
    expect(pending()).toBeNull();
  });

  it('starts empty in the next run, and an answer of the run before does not touch it', async () => {
    writes.write(OFFSET, 50);
    const [first] = requests();
    writes.write(OFFSET, 60); // A held value: not sent to the next run
    await deliver(hello(2));
    expect(status()).toEqual({ kind: 'idle' });
    expect(pending()).toBeNull();
    first.flush({ session_id: 1, operation: 4 });
    await turn();
    expect(requests()).toHaveLength(0);
    expect(status()).toEqual({ kind: 'idle' });
    expect(pending()).toBeNull();
    const next = expectNextIsSent(70);
    expect(next.request.body).toMatchObject({ session: 2 });
  });

  it('gives up a write that waits for its result when the next run starts', async () => {
    writes.write(OFFSET, 50);
    requests()[0].flush({ session_id: 1, operation: 4 });
    writes.reject(OFFSET, 'not a number');
    await deliver(hello(2));
    expect(status()).toEqual({ kind: 'idle' });
    expect(pending()).toBeNull();
    expectNextIsSent(70);
  });

  it('keeps waiting for its own open write after a reconnection with many operations after it', async () => {
    // A write while paused: does not settle until time advances
    writes.write(OFFSET, 50);
    requests()[0].flush({ session_id: 1, operation: 1 });
    await deliver(operation(1, 'queued', false));
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // 39 operations followed meanwhile: the summary has the unsettled 1 and the last 32. The boundary is 9
    const kept = [listed(1, 'queued', false)];
    for (let seq = 9; seq <= 40; seq++) kept.push(listed(seq, 'applied', true));
    await deliver(
      hello(1, { operations: kept, operations_from_seq: 9, operations_open_omitted: 0 }),
    );
    expect(status()).toEqual({ kind: 'queued', seq: 1 });
    expect(pending()).toBe(50);
    await deliver(operation(1, 'applied', true));
    expect(status()).toEqual({ kind: 'applied', atMs: 200 });
    expect(pending()).toBeNull();
  });

  it('gives up a write whose result was lost while the connection was down', async () => {
    writes.write(OFFSET, 50);
    requests()[0].flush({ session_id: 1, operation: 1 });
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // While disconnected, 1 settled and left the summary (not in the list and older than the boundary)
    const kept = [];
    for (let seq = 9; seq <= 40; seq++) kept.push(listed(seq, 'applied', true));
    await deliver(
      hello(1, { operations: kept, operations_from_seq: 9, operations_open_omitted: 0 }),
    );
    expect(status()).toMatchObject({ kind: 'no-answer' });
    expect(pending()).toBeNull();
    expectNextIsSent(60);
  });
});
