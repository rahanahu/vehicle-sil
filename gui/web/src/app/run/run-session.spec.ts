import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { FakeWebSockets, HttpExample, httpExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { SimulationTime } from '../core/session/simulation-time';
import { RunSession } from './run-session';

// 包みの試験: HttpClient（トークンつき）で送ること、応答がその場で signal に出ること。開始と停止の決まり
// （開始中の停止、断られた理由をいつまで出すか）は gui/client の試験（run.spec.ts）。
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
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: new FakeWebSockets().factory },
      ],
    });
    http = TestBed.inject(HttpTestingController);
    run = TestBed.inject(RunSession);
  });

  afterEach(() => sessionStorage.clear());

  it('starts, and shows the new session from the answer at once', () => {
    run.start({ disable: ['B'], speed: 'max', start_paused: false }, 'a.yaml');
    expect(run.starting()).toBe(true);
    const request = http.expectOne('api/run');
    expect(request.request.headers.get('Authorization')).toBe('Bearer secret');
    expect(request.request.body).toEqual({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
    });
    request.flush(examples.get('run')?.response as object);
    expect(run.starting()).toBe(false);
    expect(run.launcherRunning()).toBe(true);
    // 正面の口の status: ランチャーはまだ段階を言っていない
    expect(run.status().run).toBe('starting');
    expect(run.status().result).toBeNull();
  });

  it('shows why the server refused', async () => {
    run.forceStop();
    expect(run.stopping()).toBe('force');
    http
      .expectOne('api/stop')
      .flush(
        { error: { code: 'shutting-down', message: 'the server is shutting down', detail: null } },
        { status: 503, statusText: 'Service Unavailable' },
      );
    expect(run.stopping()).toBeNull();
    await new Promise((resolve) => setTimeout(resolve));
    expect(run.failure()).toEqual({
      op: 'force_stop',
      problem: { code: 'shutting-down', message: 'the server is shutting down' },
    });
  });

  it('leaves the failure of a time command to the time card', async () => {
    run.stop();
    http
      .expectOne('api/stop')
      .flush(
        { error: { code: 'shutting-down', message: 'the server is shutting down', detail: null } },
        { status: 503, statusText: 'Service Unavailable' },
      );
    await new Promise((resolve) => setTimeout(resolve));
    expect(run.failure()?.op).toBe('stop');
    // 時刻の命令が最後の操作になる: 停止の失敗は消え（正面の口が次の操作で消す）、時刻の命令の失敗は帯の下に出ない
    const time = TestBed.inject(SimulationTime);
    const sent = time.setSpeed(2);
    expect(run.failure()).toBeNull();
    http
      .expectOne('api/time')
      .flush(
        { error: { code: 'not-running', message: 'no run is going', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    await sent;
    expect(time.failure()).toMatchObject({ op: 'speed', problem: { code: 'not-running' } });
    expect(run.failure()).toBeNull();
  });
});
