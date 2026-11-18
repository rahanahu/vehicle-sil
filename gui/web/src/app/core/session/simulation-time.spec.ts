import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { FakeWebSockets, httpExamples, webSocketExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../auth/auth-interceptor';
import { SOCKET_FACTORY } from './gui-client';
import { ServerInfo } from './server-info';
import { ANSWER_TIMEOUT, SimulationTime } from './simulation-time';

/** 試験の中の、ランチャーの答えを待つ時間（本番より短く）。 */
const ANSWER_MS = 1000;

// 包みの試験: HttpClient（トークンつき）で送ること、状態が signal に写ること、知らせを画面の言葉にすること。
// いつ押せるか、いつ待つのをやめるか、は gui/client の試験（time.spec.ts）。
describe('SimulationTime', () => {
  let fake: FakeWebSockets;
  let http: HttpTestingController;
  let server: Map<string, unknown[]>;
  let app: object;

  beforeAll(async () => {
    server = new Map((await webSocketExamples()).map((e) => [e.name, e.server]));
    app = (await httpExamples()).find((e) => e.name === 'app')?.response as object;
  });

  beforeEach(() => {
    vi.useFakeTimers();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    TestBed.configureTestingModule({
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
        { provide: ANSWER_TIMEOUT, useValue: ANSWER_MS },
      ],
    });
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => {
    vi.useRealTimers();
    sessionStorage.clear();
  });

  /** サービスを作り、サーバーの情報に答えて、WebSocket を開き、一時停止したところまで流す。 */
  function openPaused(): SimulationTime {
    const time = TestBed.inject(SimulationTime);
    TestBed.inject(ServerInfo).features();
    TestBed.tick();
    http.expectOne('api/app').flush(app);
    fake.last().open();
    for (const message of server.get('ws-time') ?? []) {
      fake.last().send(message);
      const event = (message as { event?: { event?: string; paused?: boolean } }).event;
      if (event?.event === 'time' && event.paused === true) break;
    }
    vi.advanceTimersByTime(BATCH_MS);
    return time;
  }

  it('follows the time of the run, and sends a command with the token', async () => {
    const time = openPaused();
    expect(time.supported()).toBe(true);
    expect([time.simMs(), time.paused(), time.speed()]).toEqual([0, true, 'max']);
    const { pause, resume, step } = time.controls();
    expect([pause.ok, resume.ok, step.ok]).toEqual([false, true, true]);

    const sent = time.step(25);
    const request = http.expectOne('api/time');
    expect(request.request.body).toMatchObject({ command: 'step', ms: 25 });
    expect(request.request.headers.get('Authorization')).toBe('Bearer secret');
    expect(time.waiting()).toBe(true);
    expect(time.controls().step.reason?.code).toBe('busy');
    request.flush({ session_id: 1, command: { command: 'step', ms: 25 } });
    expect(time.waiting()).toBe(true);
    // 答えを待つ間に同じ命令を送っても、2 つ目は送らない
    await time.step(25);
    http.expectNone('api/time');
    expect(time.error()).toContain('answered');
    vi.advanceTimersByTime(ANSWER_MS);
    await sent;
  });

  it('says in the words of the screen that the launcher did not answer', async () => {
    const time = openPaused();
    const sent = time.step(10);
    http.expectOne('api/time').flush({ session_id: 1, command: { command: 'step', ms: 10 } });
    await vi.advanceTimersByTimeAsync(ANSWER_MS);
    await sent;
    expect(time.waiting()).toBe(false);
    expect(time.error()).toContain('答えません');
    expect(time.error()).toContain('step');
    expect(time.errorFromServer()).toBe(false);
  });

  it('shows the words of the server when it refused a command', async () => {
    const time = openPaused();
    const sent = time.resume();
    http
      .expectOne('api/time')
      .flush(
        { error: { code: 'not-paused', message: 'the simulation is not paused', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    await sent;
    expect(time.error()).toBe('the simulation is not paused');
    expect(time.errorFromServer()).toBe(true);
  });

  it('offers no command to a launcher without time commands', () => {
    const time = TestBed.inject(SimulationTime);
    TestBed.inject(ServerInfo).features();
    TestBed.tick();
    const answer = structuredClone(app) as { launcher: { version: { features: string[] } } };
    answer.launcher.version.features = ['events', 'stdin-commands'];
    http.expectOne('api/app').flush(answer);
    expect(time.supported()).toBe(false);
    expect(TestBed.inject(ServerInfo).has('events')).toBe(true);
  });
});
