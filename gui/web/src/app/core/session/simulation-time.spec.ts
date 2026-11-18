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

// 包みの試験: HttpClient（トークンつき）で送ること、状態が signal に写ること、失敗が最後の命令と組になること。
// いつ押せるか、いつ待つのをやめるか、失敗をいつ消すか、は gui/client の試験（time.spec.ts、front.spec.ts）。
// 失敗の文は labels.spec.ts。
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

  interface Event {
    event?: string;
    sim_ms?: number;
    phase?: string;
  }

  /** サービスを作り、サーバーの情報に答えて、WebSocket を開き、`last` が真になる事象まで流す。 */
  function openUntil(last: (event: Event) => boolean): SimulationTime {
    const time = TestBed.inject(SimulationTime);
    TestBed.inject(ServerInfo);
    TestBed.tick();
    http.expectOne('api/app').flush(app);
    fake.last().open();
    for (const message of server.get('ws-time') ?? []) {
      fake.last().send(message);
      const event = (message as { event?: Event }).event;
      if (event && last(event)) break;
    }
    vi.advanceTimersByTime(BATCH_MS);
    return time;
  }

  /** 止めて始めた実行: state が一時停止と言い、その後の time が時刻 0 を言うところまで。 */
  const openPaused = () => openUntil((e) => e.event === 'time' && e.sim_ms === 0);

  it('says that a run started paused is paused while it waits for the participants', () => {
    const time = openUntil((e) => e.event === 'state' && e.phase === 'waiting');
    expect(time.paused()).toBe(true);
    expect(time.speed()).toBe('max');
  });

  it('follows the time of the run, and sends a command with the token', async () => {
    const time = openPaused();
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
    expect(time.failure()).toMatchObject({ op: 'step', problem: { code: 'busy' } });
    vi.advanceTimersByTime(ANSWER_MS);
    await sent;
  });

  it('pairs the last command with the problem when the launcher did not answer', async () => {
    const time = openPaused();
    const sent = time.step(10);
    http.expectOne('api/time').flush({ session_id: 1, command: { command: 'step', ms: 10 } });
    await vi.advanceTimersByTimeAsync(ANSWER_MS);
    await sent;
    expect(time.waiting()).toBe(false);
    expect(time.failure()).toMatchObject({ op: 'step', problem: { code: 'no-answer' } });
    expect(time.answerTimeoutMs).toBe(ANSWER_MS);
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
    expect(time.failure()).toEqual({
      op: 'resume',
      problem: { code: 'not-paused', message: 'the simulation is not paused' },
    });
  });
});
