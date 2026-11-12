import { GuiClient, createClient } from './client';
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

  /** 試験が読むもの（実行の操作とセッションの写しの読める値を、呼べば値が返る形にしたもの）。 */
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
    return example.response as object;
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
    run.stop();
    expect(http.expectOne('api/stop').request.body).toEqual({ force: false });
    run.forceStop();
    expect(http.expectOne('api/stop').request.body).toEqual({ force: true });
    http.verify();
  });

  it('shows why the server refused', () => {
    run.start({ file: 'a.yaml', disable: [], speed: null, start_paused: false });
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'session-running', message: 'a simulation is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(run.starting()).toBe(false);
    expect(run.actionError()).toBe('a simulation is running');
    run.stop();
    http.expectOne('api/stop').flush({ session: null });
    expect(run.actionError()).toBeNull();
  });

  it('can be asked to stop while the start waits for its answer, and asks again if the answer does not show it', () => {
    run.start({ file: 'a.yaml', disable: [], speed: null, start_paused: false });
    run.stop();
    expect(run.stopWhileStarting()).toBe(true);
    expect(run.stopping()).toBe('stop');
    // 停止の応答は、まだ前の実行（ここでは何もない）の要約
    http.expectOne('api/stop').flush({ session: null });
    expect(run.stopping()).toBeNull();
    // 開始の応答の要約は「頼んでいない」: 何度頼んでも同じ停止を、もう一度頼む
    http.expectOne('api/run').flush(answer('run'));
    expect(run.stopWhileStarting()).toBe(false);
    expect(http.expectOne('api/stop').request.body).toEqual({ force: false });
    http.verify();
  });

  it('says so when an answer cannot be read, instead of dropping it silently', () => {
    run.stop();
    http.expectOne('api/stop').flush({ session: { id: 'x' } });
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
    run.stop();
    http.expectOne('api/stop').flush(cut);
    expect(run.outcome()).toBe('ended');
    expect(run.session()?.result?.result).toBe('passed');
    expect(run.session()?.unreadable).toEqual(['result.participants', 'result.findings']);
  });

  it('forgets a refusal once another run starts or the run ends', () => {
    run.start({ file: 'a.yaml', disable: [], speed: null, start_paused: false });
    http
      .expectOne('api/run')
      .flush(
        { error: { code: 'already-running', message: 'a run is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(run.actionError()).toBe('a run is running');
    // 別の画面が始めた実行の要約が届いた
    void client.fetchSession();
    http.expectOne('api/session').flush(answer('run'));
    expect(run.actionError()).toBeNull();
  });

  it('says that no answer came, not that the start failed, and waits as long as the server may check', () => {
    let rejected: unknown = null;
    client.run
      .start({ file: 'a.yaml', disable: [], speed: null, start_paused: false })
      .catch((e) => {
        rejected = e;
      });
    const request = http.expectOne('api/run');
    // 開始の中で照合し直すことがある: 順番待ちと照合の 2 つ分と、余裕
    expect(request.request.timeoutMs).toBe(2 * 120 * 1000 + DEFAULT_TIMEOUT_MS);
    request.fail(HttpError.noAnswer('POST', 'api/run', request.request.timeoutMs ?? 0));
    expect(run.starting()).toBe(false);
    expect(run.actionError()).toContain('no answer from POST api/run');
    expect(run.actionError()).toContain('may still start the run');
    return Promise.resolve().then(() => expect((rejected as HttpError).timedOut).toBe(true));
  });

  it('stops with the usual wait', () => {
    run.stop();
    expect(http.expectOne('api/stop').request.timeoutMs).toBeUndefined();
  });
});
