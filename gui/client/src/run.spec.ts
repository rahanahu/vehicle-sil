import { GuiClient, createClient } from './client';
import { BATCH_MS, backoffMs } from './connection';
import { DEFAULT_TIMEOUT_MS, HttpError } from './http';
import { runOutcome } from './session-state';
import { FakeHttp } from './testing/fake-http';
import { FakeWebSockets } from './testing/fake-web-socket';
import { HttpExample, httpExamples } from './testing/protocol-examples';

describe('RunControl', () => {
  let examples: Map<string, HttpExample>;
  let http: FakeHttp;
  let client: GuiClient;
  let run: ReturnType<typeof view>;

  /**
   * What the tests read (the readable values of run control and the session copy, as functions that return the value).
   */
  function view(c: GuiClient) {
    return {
      start: (request: Parameters<GuiClient['run']['start']>[0]) => {
        c.run.start(request).catch(() => undefined);
      },
      stop: () => {
        c.run.stop().catch(() => undefined);
      },
      forceStop: () => {
        c.run.forceStop().catch(() => undefined);
      },
      starting: () => c.run.starting.get(),
      stopping: () => c.run.stopping.get(),
      stopWhileStarting: () => c.run.stopWhileStarting.get(),
      actionError: () => c.run.error.get(),
      session: () => c.session.session.get(),
      launcherRunning: () => c.session.session.get()?.launcher.running ?? false,
      outcome: () => {
        const session = c.session.session.get();
        return session ? runOutcome(session) : null;
      },
    };
  }

  beforeAll(async () => {
    examples = new Map((await httpExamples()).map((example) => [example.name, example]));
  });

  beforeEach(() => {
    http = new FakeHttp();
    client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: new FakeWebSockets().factory,
    });
    run = view(client);
  });

  afterEach(() => client.close());

  function answer(name: string): object {
    const example = examples.get(name);
    if (!example) throw new Error(`no example ${name}`);
    return { ...(example.response as object), instance: 'test-instance' };
  }

  function seedSession(): number {
    void client.fetchSession();
    http.expectOne('api/session').flush(answer('run'));
    return (answer('run') as { session: { id: number } }).session.id;
  }

  it('starts, and shows the new session from the answer at once', () => {
    run.start({ file: 'a.yaml', disable: ['B'], speed: 'max', start_paused: false });
    expect(run.starting()).toBe(true);
    const request = http.expectOne('api/run');
    expect(request.request.body).toEqual({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
      start_paused: false,
    });
    request.flush(answer('run'));
    expect(run.starting()).toBe(false);
    expect(run.launcherRunning()).toBe(true);
    expect(run.outcome()).toBe('running');
  });

  it('asks to stop in order, and only the other button asks to stop without waiting', () => {
    const id = seedSession();
    run.stop();
    expect(http.expectOne('api/stop-session').request.body).toEqual({
      force: false,
      session: id,
      instance: 'test-instance',
    });
    run.forceStop();
    expect(http.expectOne('api/stop-session').request.body).toEqual({
      force: true,
      session: id,
      instance: 'test-instance',
    });
    http.verify();
  });

  it('keeps the original stop target when a stopping subscriber changes the session', async () => {
    const id = seedSession();
    const unsubscribe = client.run.stopping.subscribe((stopping) => {
      if (!stopping) return;
      void client.fetchSession();
      const response = answer('run') as { session: Record<string, unknown> };
      http
        .expectOne('api/session')
        .flush({ ...response, session: { ...response.session, id: id + 1 } });
    });
    const stopped = client.run.stop();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ force: false, session: id, instance: 'test-instance' });
    request.flush(answer('run'));
    await stopped;
    unsubscribe();
  });

  it('refuses an overlapping start without changing the first start target', async () => {
    const first = client.run.start({ file: 'a.yaml', disable: [] });
    const second = client.run.start({ file: 'b.yaml', disable: [] });
    await expect(second).rejects.toThrow('already pending');
    const stopping = client.run.stop();
    http.expectOne('api/run').flush(answer('run'));
    await first;
    await Promise.resolve();
    const request = http.expectOne('api/stop-session');
    request.flush(answer('run'));
    await stopping;
  });

  it('keeps instance and session paired during a synchronous hello subscriber', async () => {
    vi.useFakeTimers();
    const sockets = new FakeWebSockets();
    const transport = new FakeHttp();
    const other = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: transport.transport,
      socket: sockets.factory,
    });
    try {
      other.connect();
      const first = sockets.last();
      first.open();
      const session = (answer('run') as { session: Record<string, unknown> }).session;
      const hello = (instance: string) => ({
        type: 'hello',
        vehicle_sil_gui: 2,
        version: '0.1.0',
        instance,
        session,
        allowed: session['allowed'],
        replay: null,
      });
      first.send(hello('old'));
      vi.advanceTimersByTime(BATCH_MS);
      first.fail();
      vi.advanceTimersByTime(backoffMs(1));
      const next = sockets.last();
      next.open();
      let stop: Promise<unknown> | null = null;
      const unsubscribe = other.session.allowed.subscribe(() => {
        if (!stop) stop = other.run.stop();
      });
      next.send(hello('new'));
      vi.advanceTimersByTime(BATCH_MS);
      const request = transport.expectOne('api/stop-session');
      expect(request.request.body).toMatchObject({ session: session['id'], instance: 'new' });
      request.flush({ session });
      await stop;
      unsubscribe();
    } finally {
      other.close();
      vi.useRealTimers();
    }
  });

  it('resynchronizes an HTTP-only client after the server restarts', async () => {
    const id = seedSession();
    const staleRead = client.fetchSession();
    const stale = http.expectOne('api/session');
    const freshRead = client.fetchSession();
    const fresh = http.expectOne('api/session');
    fresh.flush({ ...answer('run'), instance: 'new-instance' });
    await freshRead;
    stale.flush(answer('run'));
    await staleRead;
    const stop = client.run.stop();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ session: id, instance: 'new-instance', force: false });
    request.flush(answer('run'));
    await stop;
  });

  it('shows why the server refused', () => {
    run.start({ file: 'a.yaml', disable: [] });
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'session-running', message: 'a simulation is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(run.starting()).toBe(false);
    expect(run.actionError()).toBe('a simulation is running');
    seedSession();
    run.stop();
    http.expectOne('api/stop-session').flush(answer('run'));
    expect(run.actionError()).toBeNull();
  });

  it('waits for a start answer and sends a stop targeted at that session', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    run.stop();
    expect(run.stopWhileStarting()).toBe(true);
    expect(run.stopping()).toBe('stop');
    http.expectNone('api/stop');
    http.expectOne('api/run').flush(answer('run'));
    await Promise.resolve();
    await Promise.resolve();
    const stop = http.expectOne('api/stop-session');
    const id = (answer('run') as { session: { id: number } }).session.id;
    expect(stop.request.body).toEqual({ force: false, session: id, instance: 'test-instance' });
    stop.flush(answer('run'));
    await Promise.resolve();
    http.verify();
  });

  it('targets the start answer even when a newer session has already arrived', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    run.stop();
    const newer = structuredClone(answer('run')) as { session: { id: number } };
    newer.session.id += 1;
    void client.fetchSession();
    http.expectOne('api/session').flush(newer);
    http.expectOne('api/run').flush(answer('run'));
    await Promise.resolve();
    await Promise.resolve();
    const stop = http.expectOne('api/stop-session');
    expect(stop.request.body).toEqual({
      force: false,
      session: newer.session.id - 1,
      instance: 'test-instance',
    });
    stop.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await Promise.resolve();
    expect(run.session()?.id).toBe(newer.session.id);
  });

  it('keeps the old server instance on a delayed start answer after a restart', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const stopped = client.run.stop();
    // The new server has reused the same numeric session ID. Its response arrives first.
    void client.fetchSession();
    http.expectOne('api/session').flush(answer('run'));
    const old = { ...answer('run'), instance: 'old-instance' };
    http.expectOne('api/run').flush(old);
    await Promise.resolve();
    await Promise.resolve();
    const request = http.expectOne('api/stop-session');
    const id = (answer('run') as { session: { id: number } }).session.id;
    expect(request.request.body).toEqual({ force: false, session: id, instance: 'old-instance' });
    request.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await expect(stopped).rejects.toMatchObject({ status: 409 });
    expect(run.session()?.id).toBe(id);
  });

  it('does not guess an instance if an older start answer does not report one', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const stopped = client.run.stop();
    const old = { ...answer('run') } as { instance?: string };
    delete old.instance;
    http.expectOne('api/run').flush(old);
    await expect(stopped).rejects.toThrow('did not identify the run');
    http.expectNone('api/stop-session');
  });

  it('does not send an unscoped stop if the start has no answer', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const stop = client.run.stop();
    http.expectOne('api/run').fail(HttpError.noAnswer('POST', 'api/run', 1000));
    await expect(stop).rejects.toThrow('no stop was sent');
    expect(run.actionError()).toContain('no stop was sent');
    http.expectNone('api/stop');
    http.expectNone('api/stop-session');
  });

  it('does not stop an unknown session', async () => {
    await expect(client.run.stop()).rejects.toThrow('no session and server instance are known');
    http.expectNone('api/stop');
    http.expectNone('api/stop-session');
  });

  it('reports that an old server cannot safely stop a session', async () => {
    seedSession();
    const stopped = client.run.stop();
    http.expectOne('api/stop-session').flush({}, { status: 404, statusText: 'Not Found' });
    await expect(stopped).rejects.toMatchObject({
      status: 404,
      problem: {
        code: 'targeted-stop-unsupported',
        message: expect.stringContaining('no stop was sent'),
      },
    });
    http.expectNone('api/stop');
  });

  it('does not report success when a stop answer names another session', async () => {
    const id = seedSession();
    const stopped = client.run.stop();
    const other = structuredClone(answer('run')) as { session: { id: number } };
    other.session.id = id + 1;
    http.expectOne('api/stop-session').flush(other);
    await expect(stopped).rejects.toThrow('stop answer was unreadable');
    expect(run.session()?.id).toBe(id);
  });

  it('keeps the known session as the target even if a newer one arrives before the answer', async () => {
    const id = seedSession();
    const stopped = client.run.stop();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ force: false, session: id, instance: 'test-instance' });
    const newer = structuredClone(answer('run')) as { session: { id: number } };
    newer.session.id = id + 1;
    void client.fetchSession();
    http.expectOne('api/session').flush(newer);
    request.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await expect(stopped).rejects.toMatchObject({ status: 409 });
    expect(run.session()?.id).toBe(id + 1);
  });

  it('targets both stops requested while the same start is pending', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const first = client.run.stop();
    const second = client.run.forceStop();
    http.expectOne('api/run').flush(answer('run'));
    await Promise.resolve();
    await Promise.resolve();
    const stops = http.requests.splice(0);
    expect(stops.map((request) => request.request.path)).toEqual([
      'api/stop-session',
      'api/stop-session',
    ]);
    const id = (answer('run') as { session: { id: number } }).session.id;
    expect(stops.map((request) => request.request.body)).toEqual([
      { force: false, session: id, instance: 'test-instance' },
      { force: true, session: id, instance: 'test-instance' },
    ]);
    stops.forEach((request) => request.flush(answer('run')));
    await Promise.all([first, second]);
  });

  it('says so when an answer cannot be read, instead of dropping it silently', () => {
    seedSession();
    run.stop();
    http.expectOne('api/stop-session').flush({ session: { id: 'x' } });
    const notices = client.session.notices.get();
    expect(notices.map((n) => n.message)).toEqual([
      'unreadable answer: response.session.id: expected an integer',
    ]);
  });

  it('reads an answer whose result the server cut to its plain fields', () => {
    const cut = structuredClone(answer('session-ended')) as {
      session: { result: Record<string, unknown> };
    };
    for (const key of ['participants', 'findings', 'check']) delete cut.session.result[key];
    seedSession();
    run.stop();
    http.expectOne('api/stop-session').flush(cut);
    expect(run.outcome()).toBe('ended');
    expect(run.session()?.result?.result).toBe('passed');
    expect(run.session()?.unreadable).toEqual(['result.participants', 'result.findings']);
  });

  it('forgets a refusal once another run starts or the run ends', () => {
    run.start({ file: 'a.yaml', disable: [] });
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'already-running', message: 'a run is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(run.actionError()).toBe('a run is running');
    // The summary of a run another screen started arrived
    void client.fetchSession();
    http.expectOne('api/session').flush(answer('run'));
    expect(run.actionError()).toBeNull();
  });

  it('says that no answer came, not that the start failed, and waits as long as the server may check', async () => {
    const started = client.run.start({ file: 'a.yaml', disable: [] });
    const request = http.expectOne('api/run');
    // A start may check again: room for waiting its turn and for the check, plus a margin
    expect(request.request.timeoutMs).toBe(2 * 120 * 1000 + DEFAULT_TIMEOUT_MS);
    request.fail(HttpError.noAnswer('POST', 'api/run', request.request.timeoutMs ?? 0));
    expect(run.starting()).toBe(false);
    expect(run.actionError()).toContain('no answer from POST api/run');
    expect(run.actionError()).toContain('may still start the run');
    await expect(started).rejects.toMatchObject({ timedOut: true });
  });

  it('stops with the usual wait', () => {
    seedSession();
    run.stop();
    expect(http.expectOne('api/stop-session').request.timeoutMs).toBeUndefined();
  });
});
