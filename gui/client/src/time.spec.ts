import { GuiClient, createClient } from './client';
import { BATCH_MS } from './connection';
import { ClientTime } from './time';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { httpExamples, webSocketExamples } from './testing/protocol-examples';

/** How long the tests wait for the launcher's answer (shorter than in production). */
const ANSWER_MS = 1000;

/** What the tests read (the fields of the time state, as functions that return the value), and the commands. */
function view(client: GuiClient) {
  const control: ClientTime = client.time;
  const s = () => control.state.get();
  return {
    simMs: () => s().time?.simMs ?? null,
    // Whether it is paused and the speed set are the launcher's state's to say (not the time's)
    speed: () => client.session.session.get()?.launcherState?.speed ?? null,
    paused: () => client.session.session.get()?.launcherState?.paused ?? null,
    paced: () => s().paced,
    lateness: () => s().lateness,
    behind: () => s().behind,
    lastCommand: () => s().lastCommand,
    sending: () => s().sending,
    error: () => s().failure?.message ?? null,
    errorFromServer: () => s().failure?.kind === 'server',
    pause: () => control.pause(),
    resume: () => control.resume(),
    step: (ms: number) => control.step(ms),
    runUntil: (atMs: number) => control.send({ command: 'run_until', at_ms: atMs }),
  };
}
type TimeView = ReturnType<typeof view>;

describe('TimeControl', () => {
  let fake: FakeWebSockets;
  let http: FakeHttp;
  let server: Map<string, unknown[]>;
  let app: unknown;
  let client: GuiClient;

  beforeAll(async () => {
    server = new Map((await webSocketExamples()).map((e) => [e.name, e.server]));
    app = (await httpExamples()).find((e) => e.name === 'app')?.response;
  });

  beforeEach(() => {
    vi.useFakeTimers();
    fake = new FakeWebSockets();
    http = new FakeHttp();
    client = createClient({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: fake.factory,
      answerTimeoutMs: ANSWER_MS,
    });
  });

  afterEach(() => {
    client.close();
    vi.useRealTimers();
  });

  /** Loads the server information and opens the WebSocket. */
  async function open(): Promise<{ time: TimeView; socket: FakeSocket }> {
    const loaded = client.loadApp();
    http.expectOne('api/app').flush(app);
    await loaded;
    client.connect();
    const socket = fake.last();
    socket.open();
    return { time: view(client), socket };
  }

  /** Feeds the example's notifications up to and including the first that matches. Returns how many were fed. */
  async function play(
    socket: FakeSocket,
    name: string,
    from: number,
    until: (m: Record<string, unknown>) => boolean = () => false,
  ): Promise<number> {
    const messages = server.get(name) ?? [];
    let i = from;
    while (i < messages.length) {
      socket.send(messages[i]);
      if (until(messages[i++] as Record<string, unknown>)) break;
    }
    vi.advanceTimersByTime(BATCH_MS);
    return i;
  }

  const timeEvent =
    (fields: Record<string, unknown>) =>
    (m: Record<string, unknown>): boolean => {
      const event = m['event'] as Record<string, unknown> | undefined;
      return (
        m['type'] === 'event' &&
        event?.['event'] === 'time' &&
        Object.entries(fields).every(([k, v]) => event[k] === v)
      );
    };

  /** A state event of the launcher with these fields. */
  const stateEvent =
    (fields: Record<string, unknown>) =>
    (m: Record<string, unknown>): boolean => {
      const event = m['event'] as Record<string, unknown> | undefined;
      return (
        m['type'] === 'event' &&
        event?.['event'] === 'state' &&
        Object.entries(fields).every(([k, v]) => event[k] === v)
      );
    };
  /** The state that says the run stands still. */
  const standing = stateEvent({ phase: 'running', paused: true });

  it('follows the time of a run and the answers to its commands', async () => {
    const { time, socket } = await open();

    // Started paused: the state says that it stands still, the time event where
    let at = await play(socket, 'ws-time', 0, timeEvent({ sim_ms: 0 }));
    expect(time.simMs()).toBe(0);
    expect(time.paused()).toBe(true);
    expect(time.speed()).toBe('max');

    time.step(25);
    const step = http.expectOne('api/time');
    expect(step.request.method).toBe('POST');
    expect(step.request.body).toEqual({ command: 'step', ms: 25 });
    expect(time.sending()).toBe('step');
    step.flush({ session_id: 1, command: { command: 'step', ms: 25 } });
    // No further command is sent until the launcher's answer (the command event) arrives (a double click does not
    // advance twice)
    expect(time.sending()).toBe('step');
    // An attempt to send while waiting sends nothing and says so (rather than dropping it silently)
    expect(time.step(25)).toEqual({ sent: false, reason: 'waiting' });
    http.expectNone('api/time');

    // The amount the launcher rounded up to the time step, and the time reached
    at = await play(socket, 'ws-time', at, timeEvent({ sim_ms: 25 }));
    expect(time.lastCommand()).toMatchObject({ name: 'step', accepted: true, ms: 25, atMs: 25 });
    expect(time.simMs()).toBe(25);
    expect(time.sending()).toBeNull();
    // The time event says where it is; the state after it says that it stands still there
    at = await play(socket, 'ws-time', at, standing);
    expect(time.paused()).toBe(true);

    // Running. At real-time speed, lateness is counted
    at = await play(socket, 'ws-time', at, stateEvent({ paused: false, speed: 1 }));
    expect(time.paused()).toBe(false);
    expect(time.speed()).toBe(1);
    expect(time.paced()).toBe(true);
    expect(time.lateness()).toEqual({ lateSteps: 0, maxLateMs: 0, slippedMs: 0, lateMs: 0 });
    expect(time.behind()).toBe(false);

    // A command the launcher refused carries the reason
    at = await play(
      socket,
      'ws-time',
      at,
      (m) => (m['event'] as Record<string, unknown> | undefined)?.['accepted'] === false,
    );
    expect(time.lastCommand()).toMatchObject({ name: 'step', accepted: false });
    expect(time.lastCommand()?.message).toContain('paused');

    // Once ended, it stands still nowhere
    await play(socket, 'ws-time', at);
    expect(time.paused()).toBe(false);
  });

  it('sends run_until with the time to pause at', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    expect(time.runUntil(500)).toEqual({ sent: true });
    const sent = http.expectOne('api/time');
    expect(sent.request.body).toEqual({ command: 'run_until', at_ms: 500 });
    expect(time.sending()).toBe('run_until');
  });

  it('does not call a small lateness within the tolerance late', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    socket.send({
      type: 'event',
      session: 1,
      event: {
        event: 'time',
        seq: 1000,
        t_ms: 0,
        sim_ms: 100,
        actual_speed: 1,
        late_steps: 0,
        max_late_ms: 3,
        slipped_ms: 0,
        late_ms: 3,
      },
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.lateness()?.lateMs).toBe(3);
    expect(time.behind()).toBe(false);
  });

  /** Sends and advances to the server's answer (200). The launcher's answer has not come yet. */
  async function sendStep(time: TimeView): Promise<void> {
    expect(time.step(10)).toEqual({ sent: true });
    http.expectOne('api/time').flush({ session_id: 1, command: { command: 'step', ms: 10 } });
    expect(time.sending()).toBe('step');
  }

  /** The last summary of ws-time (an ended run), copied with changes. */
  function lastSummary(change: (s: Record<string, unknown>) => void): Record<string, unknown> {
    const messages = server.get('ws-time') ?? [];
    const last = structuredClone(
      [...messages].reverse().find((m) => (m as { type: string }).type === 'session'),
    ) as { session: Record<string, unknown> };
    change(last.session);
    return last.session;
  }

  const running = (s: Record<string, unknown>) => {
    s['state'] = 'running';
    s['phase'] = 'running';
    s['result'] = null;
    s['stop'] = 'none';
    s['launcher'] = { pid: 1, running: true, exit_code: null, signal: null, silent_s: null };
  };

  it('stops waiting for an answer when the launcher has ended', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    await sendStep(time);
    socket.send({ type: 'session', session: lastSummary((s) => (s['last_command'] = null)) });
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBeNull();
  });

  it('stops waiting for an answer when another run has started', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    await sendStep(time);
    const next = lastSummary((s) => {
      running(s);
      s['id'] = 2;
      s['last_command'] = null;
    });
    socket.send({ type: 'session', session: next });
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBeNull();
  });

  it('takes the answer from the summary of a reconnection', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    await sendStep(time);
    const hello = (seq: number) => ({
      type: 'hello',
      vehicle_sil_gui: 3,
      instance: '0123456789abcdef',
      replay: { from_seq: null },
      session: lastSummary((s) => {
        running(s);
        s['last_command'] = {
          seq,
          command: 'step',
          source: 'stdin',
          accepted: true,
          message: null,
          ms: 10,
        };
      }),
    });
    // A summary with only an answer from before the send: keep waiting
    const before = client.session.session.get()?.lastCommand?.seq ?? -1;
    socket.send(hello(before));
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBe('step');
    // A summary with an answer from after the send: stop waiting
    socket.send(hello(before + 50));
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBeNull();
  });

  it('stops waiting after a while when the launcher does not answer, and says so', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    await sendStep(time);
    vi.advanceTimersByTime(ANSWER_MS);
    expect(time.sending()).toBeNull();
    expect(time.error()).toContain('did not answer the command (step) within 1 s');
    expect(time.errorFromServer()).toBe(false);
  });

  it('says why the server refused a command, for the run it was sent to', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    time.resume();
    http
      .expectOne('api/time')
      .flush(
        { error: { code: 'not-running', message: 'no simulation is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(time.error()).toBe('no simulation is running');
    expect(time.errorFromServer()).toBe(true);
    expect(time.sending()).toBeNull(); // Not sent: no answer to wait for
    time.pause();
    expect(time.error()).toBeNull();
    http.expectOne('api/time').flush({ session_id: 1, command: { command: 'pause' } });
  });

  it('counts the lateness of a run that fell behind real time', async () => {
    const { time, socket } = await open();
    const at = await play(socket, 'ws-late', 0, (m) => {
      const event = m['event'] as Record<string, unknown> | undefined;
      return event?.['event'] === 'time' && (event['late_ms'] as number) > 0;
    });
    expect(time.lateness()).toEqual({ lateSteps: 1, maxLateMs: 200, slippedMs: 200, lateMs: 200 });
    expect(time.behind()).toBe(true);
    await play(socket, 'ws-late', at, timeEvent({ late_ms: 0 }));
    expect(time.lateness()?.lateMs).toBe(0);
    expect(time.behind()).toBe(true);
  });

  it('stops waiting for an answer when the client is closed', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, standing);
    await sendStep(time);
    client.close();
    expect(time.sending()).toBeNull();
  });
});
