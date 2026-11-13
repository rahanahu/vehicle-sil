import { createClient } from './client';
import { BATCH_MS } from './connection';
import { FakeHttp, FakeRequest } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { BoundVariable, VariableBinding } from './variable-binding';
import { VariableRef } from './variables';

// 書き込みの様子（client.variables.bind の writeStatus と pending）の遷移を、順序を作って確かめる: 送った →
// 受け取られた → 確定、の間に、送る前に断る入力（reject）、次の値、実行の替わり目、つなぎ直しが挟まっても、
// 「送った値」が残らず、次の書き込みが送られること。サーバーの通知は偽の WebSocket で、api/write は偽の HTTP で答える。

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

const hello = (id = 1, more: object = {}) => ({
  type: 'hello',
  vehicle_sil_gui: 1,
  version: '0.1.0',
  instance: 'x',
  session: {
    id,
    state: 'running',
    launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
    stop: 'none',
    variables: variables('ready', more),
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
/** 要約の operations の 1 件。 */
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
  /** 購読者に最後に届いた様子（まだ届いていなければ、つないだときの様子）。 */
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

  /** Promise の続き（書き込みの確定）を済ませる。 */
  const turn = () => vi.advanceTimersByTimeAsync(0);

  /** まだ答えていない api/write を取り出す。 */
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
   * いまの様子。読む前に、購読者に届いている様子が同じであることを確かめる: get() は読むたびに作り直すので、
   * 知らせの漏れ（購読している画面だけが古い様子のまま残る）は、get() だけを見ていては見つからない。
   */
  const now = () => {
    const before = told;
    const fresh = bound.state.get();
    expect(before, 'the subscribers were not told of the last change').toEqual(fresh);
    return fresh;
  };
  const status = () => now().writeStatus;
  const pending = () => now().pending;
  /** 試験の本文は、移す前の形（writes.write(ref, value)）のまま: 同じ変数の bind を通す。 */
  const writes = {
    write: (_ref: VariableRef, value: number) => bound.write(value),
    reject: (_ref: VariableRef, message: string) => bound.reject(message),
  };

  /** 次の書き込みが、覚えられずにすぐ送られる（前の書き込みで止まっていない）。 */
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

  // 順序 A: 受け付けられて結果を待つ間に、送る前に断る入力をする
  for (const [result, state] of [
    ['applied', 'applied'],
    ['failed', 'late'],
  ] as const) {
    it(`drops the value sent when a write taken before an input was refused is ${result}`, async () => {
      writes.write(OFFSET, 50);
      requests()[0].flush({ session_id: 1, operation: 4 });
      await deliver(operation(4, 'queued', false));
      writes.reject(OFFSET, 'not a number');
      // 入力の誤りは、待っている書き込みに触れない: 送った値はまだ出す
      expect(status()).toEqual({ kind: 'invalid', message: 'not a number' });
      expect(pending()).toBe(50);
      await deliver(operation(4, state, true));
      expect(pending()).toBeNull();
      // 様子の欄は、最後の操作（断った入力）のまま
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

  // 順序 B: 送っている途中（サーバーの答えの前）に、送る前に断る入力をし、その後で書き込みが断られる
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
    // 先の書き込みの結果は、後の書き込みの様子と送った値に触れない
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
    writes.write(OFFSET, 60); // 4 は受け取られているので、すぐ送る
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
    writes.write(OFFSET, 60); // 覚えた値: 次の実行には送らない
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
    // 止まっている間の書き込み: 進めるまで確定しない
    writes.write(OFFSET, 50);
    requests()[0].flush({ session_id: 1, operation: 1 });
    await deliver(operation(1, 'queued', false));
    socket().fail();
    await vi.advanceTimersByTimeAsync(500);
    socket().open();
    // 間に 39 件の操作が続いた: 要約は、未確定の 1 と、最後の 32 件。境目は 9
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
    // 切れている間に 1 は確定し、要約から外れた（一覧になく、境目より古い）
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
