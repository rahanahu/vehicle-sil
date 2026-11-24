import { GuiClient, createClient } from './client';
import { BATCH_MS, backoffMs } from './connection';
import { DEFAULT_TIMEOUT_MS, HttpError } from './http';
import { runOutcome } from './session-state';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { HttpExample, httpExamples } from './testing/protocol-examples';

describe('RunControl', () => {
  let examples: Map<string, HttpExample>;
  let http: FakeHttp;
  let sockets: FakeWebSockets;
  let socket: FakeSocket;
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
    vi.useFakeTimers();
    http = new FakeHttp();
    sockets = new FakeWebSockets();
    client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: sockets.factory,
    });
    client.connect();
    socket = sockets.last();
    socket.open();
    run = view(client);
  });

  afterEach(() => {
    client.close();
    vi.useRealTimers();
  });

  function runningSummary(id = '1'): Record<string, unknown> {
    const example = examples.get('session-running');
    if (!example) throw new Error('no example session-running');
    const s = (example.response as { session: Record<string, unknown> }).session;
    return { ...s, id };
  }

  function runAck(id = '1', instance = 'test-instance') {
    return { instance, run_id: id };
  }

  function seedSession(id = '1', instance = 'test-instance'): string {
    const summary = runningSummary(id);
    socket.send({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance,
      stream_seq: 1,
      session: summary,
      allowed: summary['allowed'],
      replay: null,
    });
    socket.send({ type: 'synced', stream_seq: 1, run_id: id, last_seq: 0 });
    vi.advanceTimersByTime(BATCH_MS);
    return id;
  }

  it('starts with an HTTP acknowledgement while WebSocket delivers the session state', async () => {
    const started = client.run.start({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
      start_paused: false,
    });
    expect(run.starting()).toBe(true);
    const request = http.expectOne('api/run');
    expect(request.request.body).toEqual({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
      start_paused: false,
    });
    request.flush(runAck('1'));
    await expect(started).resolves.toEqual({ instance: 'test-instance', run_id: '1' });
    expect(run.starting()).toBe(false);
    // WebSocket stream delivers the authoritative session summary
    seedSession('1');
    expect(run.launcherRunning()).toBe(true);
    expect(run.outcome()).toBe('running');
  });

  it('preserves the previous run screen when POST api/run fails with 422 check-failed', async () => {
    seedSession('run-prev');
    expect(run.session()?.id).toBe('run-prev');
    run.start({ file: 'broken.yaml', disable: [] });
    const checkFailed = examples.get('error-check-failed')?.response;
    http
      .expectOne('api/run')
      .flush(checkFailed, { status: 422, statusText: 'Unprocessable Entity' });
    await Promise.resolve();
    expect(run.starting()).toBe(false);
    expect(run.session()?.id).toBe('run-prev');
    expect(run.actionError()).toBeTruthy();
  });

  it('asks to stop in order, and only the other button asks to stop without waiting', () => {
    const id = seedSession();
    run.stop();
    expect(http.expectOne('api/stop-session').request.body).toEqual({
      force: false,
      run_id: id,
      instance: 'test-instance',
    });
    run.forceStop();
    expect(http.expectOne('api/stop-session').request.body).toEqual({
      force: true,
      run_id: id,
      instance: 'test-instance',
    });
    http.verify();
  });

  it('keeps the original stop target when a stopping subscriber observes a new session', async () => {
    const id = seedSession('1');
    const unsubscribe = client.run.stopping.subscribe((stopping) => {
      if (!stopping) return;
      socket.send({
        type: 'session',
        stream_seq: 2,
        session: runningSummary('2'),
      });
      vi.advanceTimersByTime(BATCH_MS);
    });
    const stopped = client.run.stop();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ force: false, run_id: id, instance: 'test-instance' });
    request.flush(runAck(id));
    await stopped;
    unsubscribe();
  });

  it('refuses an overlapping start without changing the first start target', async () => {
    const first = client.run.start({ file: 'a.yaml', disable: [] });
    const second = client.run.start({ file: 'b.yaml', disable: [] });
    await expect(second).rejects.toThrow('already pending');
    const stopping = client.run.stop();
    http.expectOne('api/run').flush(runAck('1'));
    await first;
    await Promise.resolve();
    const request = http.expectOne('api/stop-session');
    request.flush(runAck('1'));
    await stopping;
  });

  it('keeps instance and run_id paired during a synchronous hello subscriber', async () => {
    const summary = runningSummary('1');
    const hello = (instance: string) => ({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance,
      stream_seq: 1,
      session: summary,
      allowed: summary['allowed'],
      replay: null,
    });
    socket.send(hello('old'));
    vi.advanceTimersByTime(BATCH_MS);
    socket.fail();
    vi.advanceTimersByTime(backoffMs(1));
    const next = sockets.last();
    next.open();
    let stop: Promise<unknown> | null = null;
    const unsubscribe = client.session.allowed.subscribe(() => {
      if (!stop) stop = client.run.stop();
    });
    next.send(hello('new'));
    vi.advanceTimersByTime(BATCH_MS);
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toMatchObject({ run_id: '1', instance: 'new' });
    request.flush(runAck('1', 'new'));
    await stop;
    unsubscribe();
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
    http.expectOne('api/stop-session').flush(runAck('1'));
    expect(run.actionError()).toBeNull();
  });

  it('waits for a start answer and sends a stop targeted at that run_id', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    run.stop();
    expect(run.stopWhileStarting()).toBe(true);
    expect(run.stopping()).toBe('stop');
    http.expectNone('api/stop');
    http.expectOne('api/run').flush(runAck('1'));
    await Promise.resolve();
    await Promise.resolve();
    const stop = http.expectOne('api/stop-session');
    expect(stop.request.body).toEqual({ force: false, run_id: '1', instance: 'test-instance' });
    stop.flush(runAck('1'));
    await Promise.resolve();
    http.verify();
  });

  it('targets the start answer even when a newer session has already arrived on WebSocket', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    run.stop();
    seedSession('2');
    http.expectOne('api/run').flush(runAck('1'));
    await Promise.resolve();
    await Promise.resolve();
    const stop = http.expectOne('api/stop-session');
    expect(stop.request.body).toEqual({
      force: false,
      run_id: '1',
      instance: 'test-instance',
    });
    stop.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await Promise.resolve();
    expect(run.session()?.id).toBe('2');
  });

  it('keeps the old server instance on a delayed start answer after a restart', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const stopped = client.run.stop();
    seedSession('1', 'test-instance');
    http.expectOne('api/run').flush(runAck('1', 'old-instance'));
    await Promise.resolve();
    await Promise.resolve();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ force: false, run_id: '1', instance: 'old-instance' });
    request.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await expect(stopped).rejects.toMatchObject({ status: 409 });
    expect(run.session()?.id).toBe('1');
  });

  it('rejects an older start answer without sending a queued stop', async () => {
    const started = client.run.start({ file: 'a.yaml', disable: [] });
    const stopped = client.run.stop();
    http.expectOne('api/run').flush({ session: runningSummary('1') });
    await expect(started).rejects.toThrow('Use the GUI server from this release');
    await expect(stopped).rejects.toThrow('no stop was sent');
    expect(run.session()).toBeNull();
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

  it('does not report success when a stop answer names another run_id', async () => {
    const id = seedSession('1');
    const stopped = client.run.stop();
    http.expectOne('api/stop-session').flush(runAck('2'));
    await expect(stopped).rejects.toThrow('stop answer was unreadable');
    expect(run.session()?.id).toBe(id);
  });

  it('keeps the known session as the target even if a newer one arrives before the answer', async () => {
    const id = seedSession('1');
    const stopped = client.run.stop();
    const request = http.expectOne('api/stop-session');
    expect(request.request.body).toEqual({ force: false, run_id: id, instance: 'test-instance' });
    socket.send({
      type: 'session',
      stream_seq: 2,
      session: runningSummary('2'),
    });
    vi.advanceTimersByTime(BATCH_MS);
    request.flush(
      { error: { code: 'session-changed', message: 'the run has changed: no stop was sent' } },
      { status: 409, statusText: 'Conflict' },
    );
    await expect(stopped).rejects.toMatchObject({ status: 409 });
    expect(run.session()?.id).toBe('2');
  });

  it('targets both stops requested while the same start is pending', async () => {
    run.start({ file: 'a.yaml', disable: [] });
    const first = client.run.stop();
    const second = client.run.forceStop();
    http.expectOne('api/run').flush(runAck('1'));
    await Promise.resolve();
    await Promise.resolve();
    const stops = http.requests.splice(0);
    expect(stops.map((request) => request.request.path)).toEqual([
      'api/stop-session',
      'api/stop-session',
    ]);
    expect(stops.map((request) => request.request.body)).toEqual([
      { force: false, run_id: '1', instance: 'test-instance' },
      { force: true, run_id: '1', instance: 'test-instance' },
    ]);
    stops.forEach((request) => request.flush(runAck('1')));
    await Promise.all([first, second]);
  });

  it('says so when a stop answer cannot be read, instead of dropping it silently', async () => {
    seedSession('1');
    const stopped = client.run.stop();
    http.expectOne('api/stop-session').flush({ instance: 'test-instance', run_id: '' });
    await expect(stopped).rejects.toThrow(
      'the stop answer was unreadable: check the session before trying again',
    );
    expect(run.actionError()).toBe(
      'the stop answer was unreadable: check the session before trying again',
    );
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
    // The summary of a run another screen started arrived over WebSocket
    seedSession('1');
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
