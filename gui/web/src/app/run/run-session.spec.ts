import { provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { readSessionResponse } from '../core/api';
import { ServerConnection } from '../core/session/server-connection';
import { FakeWebSockets } from '../core/streams/fake-web-socket';
import { HttpExample, httpExamples } from '../testing/protocol-examples';
import { RunSession } from './run-session';

describe('RunSession', () => {
  let examples: Map<string, HttpExample>;
  let http: HttpTestingController;
  let run: RunSession;

  beforeAll(async () => {
    examples = new Map((await httpExamples()).map((example) => [example.name, example]));
  });

  beforeEach(() => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    TestBed.configureTestingModule({
      providers: [
        RunSession,
        provideHttpClient(),
        provideHttpClientTesting(),
        new FakeWebSockets().provider,
      ],
    });
    http = TestBed.inject(HttpTestingController);
    run = TestBed.inject(RunSession);
  });

  afterEach(() => sessionStorage.clear());

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
    const notices = TestBed.inject(ServerConnection).notices();
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
    TestBed.inject(ServerConnection).adopt(readSessionResponse(answer('run')));
    expect(run.actionError()).toBeNull();
  });
});
