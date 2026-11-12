import { provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
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
    expect(run.pending()).toBe('start');
    const request = http.expectOne('api/run');
    expect(request.request.body).toEqual({
      file: 'a.yaml',
      disable: ['B'],
      speed: 'max',
      start_paused: false,
    });
    request.flush(answer('run'));
    expect(run.pending()).toBeNull();
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
    expect(run.pending()).toBeNull();
    expect(run.actionError()).toBe('a simulation is running');
    run.stop();
    http.expectOne('api/stop').flush({ session: null });
    expect(run.actionError()).toBeNull();
  });
});
