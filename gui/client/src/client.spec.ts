import { DEFAULT_LIMITS, createClient, defaultOrigin, isNode } from './client';
import { BATCH_MS } from './connection';
import { DEFAULT_TIMEOUT_MS, HttpError, fetchTransport } from './http';
import { FakeHttp } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { httpExamples } from './testing/protocol-examples';

/** Fake fetch. Keeps the URLs and options it was called with, and returns the chosen answer. */
function fakeFetch(answer: () => Promise<Response>) {
  const calls: { url: string; init: RequestInit }[] = [];
  const fetch = ((url: string, init: RequestInit) => {
    calls.push({ url, init });
    return answer();
  }) as typeof globalThis.fetch;
  return { calls, fetch };
}

const json = (body: unknown, status = 200, statusText = 'OK') =>
  Promise.resolve(
    new Response(JSON.stringify(body), {
      status,
      statusText,
      headers: { 'Content-Type': 'application/json' },
    }),
  );

describe('fetchTransport', () => {
  it('sends the token, and on a POST the JSON body and the Origin it was given', async () => {
    const { calls, fetch } = fakeFetch(() => json({ session: null }));
    const send = fetchTransport({
      baseUrl: 'http://127.0.0.1:8765/gui/',
      token: 'secret',
      origin: 'http://127.0.0.1:8765',
      fetch,
    });
    const outcome = await new Promise((done) =>
      send({ method: 'POST', path: 'api/stop', body: { force: false } }, done),
    );
    expect(outcome).toEqual({ ok: true, body: { session: null } });
    expect(calls[0].url).toBe('http://127.0.0.1:8765/gui/api/stop');
    expect(calls[0].init).toMatchObject({
      method: 'POST',
      body: '{"force":false}',
      headers: {
        Authorization: 'Bearer secret',
        'Content-Type': 'application/json',
        Origin: 'http://127.0.0.1:8765',
      },
    });
    await new Promise((done) => send({ method: 'GET', path: 'api/app' }, done));
    expect(calls[1].init).toMatchObject({
      method: 'GET',
      headers: { Authorization: 'Bearer secret' },
      // Do not follow redirects (never send the token elsewhere)
      redirect: 'error',
    });
    expect(calls[1].init.headers).not.toHaveProperty('Origin');
    expect(calls[0].init).not.toHaveProperty('keepalive');
    expect(calls[1].init).not.toHaveProperty('keepalive');
  });

  it('forwards optional keepalive to fetch()', async () => {
    const { calls, fetch } = fakeFetch(() => json({ run_id: '1', operation: 1 }));
    const send = fetchTransport({
      baseUrl: 'http://127.0.0.1:8765/',
      token: 'secret',
      fetch,
    });
    await new Promise((done) =>
      send({ method: 'POST', path: 'api/write', body: {}, keepalive: true }, done),
    );
    await new Promise((done) =>
      send({ method: 'POST', path: 'api/write', body: {}, keepalive: false }, done),
    );
    expect(calls[0].init.keepalive).toBe(true);
    expect(calls[1].init.keepalive).toBe(false);
  });

  it('turns an aborted request into status 0 with the time it waited', async () => {
    vi.useFakeTimers();
    try {
      const send = fetchTransport({
        baseUrl: 'http://127.0.0.1:1/',
        token: 't',
        timeoutMs: 1000,
        fetch: ((_url: string, init: RequestInit) =>
          new Promise<Response>((_resolve, reject) =>
            init.signal?.addEventListener('abort', () => reject(new Error('aborted'))),
          )) as typeof globalThis.fetch,
      });
      const outcome = new Promise<{ ok: boolean; error?: HttpError }>((done) =>
        send({ method: 'POST', path: 'api/run', body: {} }, done),
      );
      await vi.advanceTimersByTimeAsync(1000);
      const { ok, error } = await outcome;
      expect(ok).toBe(false);
      expect([error?.status, error?.timedOut, error?.message]).toEqual([
        0,
        true,
        'no answer from POST api/run within 1 s',
      ]);
    } finally {
      vi.useRealTimers();
    }
  });

  it('takes a time-out while the body comes as no answer, not as an unreadable body', async () => {
    vi.useFakeTimers();
    try {
      const send = fetchTransport({
        baseUrl: 'http://127.0.0.1:1/',
        token: 't',
        timeoutMs: 1000,
        // The headers arrived, but the body never finishes arriving
        fetch: ((_url: string, init: RequestInit) =>
          Promise.resolve({
            ok: true,
            status: 200,
            statusText: 'OK',
            json: () =>
              new Promise((_resolve, reject) =>
                init.signal?.addEventListener('abort', () => reject(new Error('aborted'))),
              ),
          } as unknown as Response)) as typeof globalThis.fetch,
      });
      const outcome = new Promise<{ ok: boolean; error?: HttpError }>((done) =>
        send({ method: 'GET', path: 'api/app' }, done),
      );
      await vi.advanceTimersByTimeAsync(1000);
      const { ok, error } = await outcome;
      expect([ok, error?.timedOut]).toEqual([false, true]);
    } finally {
      vi.useRealTimers();
    }
  });

  it('is not a time-out when the server could not be reached', () => {
    expect(new HttpError(0, 'fetch failed', null).timedOut).toBe(false);
    expect(HttpError.noAnswer('POST', 'api/run', 2000).message).toBe(
      'no answer from POST api/run within 2 s',
    );
  });

  it('turns the server error form into an HttpError with its message, and a lost server into status 0', async () => {
    const refused = (await httpExamples()).find((e) => e.name === 'error-already-running');
    const answers = [
      () => json(refused?.response, 409, 'Conflict'),
      () => Promise.resolve(new Response('not json', { status: 502, statusText: 'Bad Gateway' })),
      () => Promise.reject(new TypeError('fetch failed')),
    ];
    const errors: HttpError[] = [];
    for (const answer of answers) {
      const { fetch } = fakeFetch(answer);
      const send = fetchTransport({ baseUrl: 'http://127.0.0.1:1/', token: 't', fetch });
      const outcome = await new Promise<{ ok: boolean; error?: HttpError }>((done) =>
        send({ method: 'POST', path: 'api/run', body: {} }, done),
      );
      expect(outcome.ok).toBe(false);
      if (outcome.error) errors.push(outcome.error);
    }
    expect(errors.map((e) => [e.status, e.problem?.code ?? null])).toEqual([
      [409, 'already-running'],
      [502, null],
      [0, null],
    ]);
    expect(errors[0].message).toBe(
      (refused?.response as { error: { message: string } }).error.message,
    );
    expect(errors[1].message).toBe('HTTP 502 Bad Gateway');
  });
});

describe('createClient', () => {
  afterEach(() => vi.unstubAllGlobals());

  it('outside a browser, sends the server origin itself (the server takes no other)', async () => {
    const app = (await httpExamples()).find((example) => example.name === 'app')?.response;
    const run = (await httpExamples()).find((example) => example.name === 'run')?.response;
    let call = 0;
    const { calls, fetch } = fakeFetch(() => json(call++ === 0 ? app : run));
    vi.stubGlobal('fetch', fetch);
    const client = createClient({ baseUrl: 'http://127.0.0.1:8765/', token: 'secret' });
    await client.loadApp();
    await client.run.start({ file: 'a.yaml', disable: [] });
    expect(calls.map((c) => (c.init.headers as Record<string, string>)['Origin'])).toEqual([
      undefined,
      'http://127.0.0.1:8765',
    ]);
    client.close();
  });

  it('knows the server and the limits as soon as api/app answers', async () => {
    const http = new FakeHttp();
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 't',
      http: http.transport,
    });
    expect(client.app.get()).toBeNull();
    expect(client.limits.get()).toEqual(DEFAULT_LIMITS);
    const loaded = client.loadApp();
    const app = (await httpExamples()).find((e) => e.name === 'app')?.response as {
      limits: { speed_max: number; step_ms_max: number };
    };
    http.expectOne('api/app').flush(app);
    // Set within the same call as the answer (without waiting for the Promise to resolve)
    expect(client.app.get()?.instance).toBe('0123456789abcdef');
    expect(client.limits.get()).toMatchObject({
      speed_max: app.limits.speed_max,
      step_ms_max: app.limits.step_ms_max,
    });
    await expect(loaded).resolves.toMatchObject({ instance: '0123456789abcdef' });
    client.close();
  });

  it('sends the Origin by default only on Node', () => {
    expect(
      defaultOrigin('http://127.0.0.1:8765/gui/', { process: { versions: { node: '24.0.0' } } }),
    ).toBe('http://127.0.0.1:8765');
    // Browsers and Web Workers (no process): the browser adds it. The second WebSocket argument is not used
    expect(defaultOrigin('http://127.0.0.1:8765/', {})).toBeUndefined();
    expect(defaultOrigin('http://127.0.0.1:8765/', { process: {} })).toBeUndefined();
    expect(isNode()).toBe(true);
  });

  it('waits for a check as long as the server lets it run, and a little more', async () => {
    const http = new FakeHttp();
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 't',
      http: http.transport,
    });
    void client.check({ file: 'a.yaml', disable: [] }).catch(() => undefined);
    // One check at a time: up to check_timeout_s for waiting its turn and up to check_timeout_s for the check
    expect(http.expectOne('api/check').request.timeoutMs).toBe(
      2 * DEFAULT_LIMITS.check_timeout_s * 1000 + DEFAULT_TIMEOUT_MS,
    );
    client.close();
  });

  it('connects again after close(), from where it was', () => {
    vi.useFakeTimers();
    try {
      const fake = new FakeWebSockets();
      const client = createClient({
        baseUrl: 'http://127.0.0.1:1/',
        token: 'secret',
        socket: fake.factory,
        http: new FakeHttp().transport,
      });
      client.connect();
      client.connect(); // A second call does nothing
      expect(fake.sockets).toHaveLength(1);
      fake.last().open();
      fake.last().send({
        type: 'hello',
        vehicle_sil_gui: 4,
        instance: 'i',
        session: null,
        replay: null,
      });
      vi.advanceTimersByTime(BATCH_MS);
      client.close();
      expect(fake.last().closedByClient).toBe(true);
      expect(client.connection.state.get()).toBe('closed');
      client.connect();
      expect(fake.sockets).toHaveLength(2);
      fake.last().open();
      expect(fake.last().greeting).toEqual([{ type: 'auth', token: 'secret' }]);
      client.close();
    } finally {
      vi.useRealTimers();
    }
  });

  it('shows only the parts that are safe to use from outside', () => {
    const client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 't',
      http: new FakeHttp().transport,
    });
    expect(Object.keys(client.connection).sort()).toEqual(
      ['failures', 'lastClose', 'onBatch', 'onUnreadable', 'state', 'tryAgainReason'].sort(),
    );
    expect(client.session).not.toHaveProperty('resumeFields');
    expect(client.time).not.toHaveProperty('dispose');
    client.close();
  });

  it('connects without a token (for --no-auth servers) and omits Authorization header', async () => {
    const { calls, fetch } = fakeFetch(() => json({ session: null }));
    const send = fetchTransport({
      baseUrl: 'http://127.0.0.1:8765/',
      fetch,
    });
    const outcome = await new Promise((done) => send({ method: 'GET', path: 'api/app' }, done));
    expect(outcome).toEqual({ ok: true, body: { session: null } });
    expect(calls[0].init.headers).not.toHaveProperty('Authorization');

    const fake = new FakeWebSockets();
    let token: string | null = null;
    const client = createClient({
      baseUrl: 'https://example.test/gui/',
      token: () => token,
      socket: fake.factory,
      http: new FakeHttp().transport,
    });
    client.connect();
    expect(fake.sockets).toHaveLength(1);
    fake.last().open();
    expect(fake.last().url).toBe('wss://example.test/gui/ws');
    expect(fake.last().greeting).toEqual([{ type: 'auth', token: '' }]);
    client.close();
    expect(fake.last().closedByClient).toBe(true);

    token = 'secret';
    client.connect();
    expect(fake.sockets).toHaveLength(2);
    fake.last().open();
    expect(fake.last().greeting).toEqual([{ type: 'auth', token: 'secret' }]);
    client.close();
    expect(fake.last().closedByClient).toBe(true);
  });

  it('rejects unknown and invalid ClientOptions fields at runtime', () => {
    expect(() =>
      createClient({ base_url: 'http://127.0.0.1:8765/' } as unknown as Parameters<
        typeof createClient
      >[0]),
    ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));
    expect(() =>
      createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        answer_timeout_ms: 1000,
      } as unknown as Parameters<typeof createClient>[0]),
    ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));
    expect(() =>
      createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        on_error: () => undefined,
      } as unknown as Parameters<typeof createClient>[0]),
    ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));
    expect(() =>
      createClient({ baseUrl: '' } as unknown as Parameters<typeof createClient>[0]),
    ).toThrow(expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }));
    expect(() => createClient({ baseUrl: 'http://127.0.0.1:8765/', answerTimeoutMs: 0 })).toThrow(
      expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }),
    );
    expect(() =>
      createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        batch_ms: 16,
      } as unknown as Parameters<typeof createClient>[0]),
    ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));
    expect(() =>
      createClient({
        baseUrl: 'http://127.0.0.1:8765/',
        watch_period_ms: 20,
      } as unknown as Parameters<typeof createClient>[0]),
    ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));
    for (const badBatch of [
      -1,
      1001,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      -Infinity,
      '16',
      null,
    ]) {
      expect(() =>
        createClient({
          baseUrl: 'http://127.0.0.1:8765/',
          batchMs: badBatch as unknown as number,
        }),
      ).toThrow(expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }));
    }
    for (const badPeriod of [
      -1,
      0,
      19,
      19.999,
      60_001,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      -Infinity,
      '20',
      null,
    ]) {
      expect(() =>
        createClient({
          baseUrl: 'http://127.0.0.1:8765/',
          watchPeriodMs: badPeriod as unknown as number,
        }),
      ).toThrow(expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }));
    }
  });

  it('applies valid batchMs and watchPeriodMs (with tier rounding and server limits clamping)', async () => {
    vi.useFakeTimers();
    try {
      const fake = new FakeWebSockets();
      const http = new FakeHttp();
      const client = createClient({
        baseUrl: 'http://127.0.0.1:1/',
        token: 'secret',
        batchMs: 16,
        watchPeriodMs: 35,
        socket: fake.factory,
        http: http.transport,
      });
      const batches: unknown[][] = [];
      client.connection.onBatch((b) => batches.push([...b]));
      client.connect();
      fake.last().open();

      // batchMs: 16 delivers non-immediate frames at 16 ms (not 15 ms)
      fake
        .last()
        .send({ type: 'hello', vehicle_sil_gui: 4, instance: 'i', session: null, replay: null });
      vi.advanceTimersByTime(15);
      expect(batches).toHaveLength(0);
      vi.advanceTimersByTime(1);
      expect(batches).toHaveLength(1);

      // watchPeriodMs: 35 rounds down via WATCH_PERIODS_MS to 20 ms for default watches and binds
      const watch = client.variables.watch([{ peer: 'Engine', name: 'engine.speed' }]);
      await vi.advanceTimersByTimeAsync(0);
      expect(fake.last().sent.filter((m) => (m as { type: string }).type === 'watch')).toEqual([
        {
          type: 'watch',
          id: 1,
          variables: [{ peer: 'Engine', name: 'engine.speed' }],
          period_ms: 20,
        },
      ]);
      watch.close();
      await vi.advanceTimersByTimeAsync(0);

      // Server limits still clamp watchPeriodMs (e.g. server min 50 ms clamps 35 -> 50)
      const app = (await httpExamples()).find((e) => e.name === 'app')?.response as {
        limits: Record<string, number>;
      };
      const load = client.loadApp();
      http
        .expectOne('api/app')
        .flush({ ...app, limits: { ...app.limits, watch_period_ms_min: 50 } });
      await load;
      const clamped = client.variables.bind({ peer: 'Engine', name: 'engine.speed' });
      await vi.advanceTimersByTimeAsync(0);
      expect(
        fake
          .last()
          .sent.filter((m) => (m as { type: string }).type === 'watch')
          .at(-1),
      ).toEqual({
        type: 'watch',
        id: 2,
        variables: [{ peer: 'Engine', name: 'engine.speed' }],
        period_ms: 50,
      });
      clamped.close();
      client.close();

      // Boundary values 0, 1000, 20, 60_000 all construct cleanly
      const fast = createClient({
        baseUrl: 'http://127.0.0.1:1/',
        batchMs: 0,
        watchPeriodMs: 20,
        socket: fake.factory,
        http: http.transport,
      });
      const slow = createClient({
        baseUrl: 'http://127.0.0.1:1/',
        batchMs: 1000,
        watchPeriodMs: 60_000,
        socket: fake.factory,
        http: http.transport,
      });
      fast.close();
      slow.close();
    } finally {
      vi.useRealTimers();
    }
  });
});
