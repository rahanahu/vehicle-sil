import { GuiClient, createClient } from './client';
import { BATCH_MS } from './connection';
import { ClientTime } from './time';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { httpExamples, webSocketExamples } from './testing/protocol-examples';

/** 試験の中の、ランチャーの答えを待つ時間（本番より短く）。 */
const ANSWER_MS = 1000;

/** 試験が読むもの（時刻の状態の欄を、呼べば値が返る形にしたもの）と、命令。 */
function view(client: GuiClient) {
  const control: ClientTime = client.time;
  const s = () => control.state.get();
  return {
    supported: () => s().supported,
    controllable: () => s().controllable,
    simMs: () => s().time?.simMs ?? null,
    speed: () => s().time?.speed ?? null,
    paused: () => s().time?.paused ?? null,
    paced: () => s().paced,
    lateness: () => s().lateness,
    behind: () => s().behind,
    lastCommand: () => s().lastCommand,
    sending: () => s().sending,
    stepping: () => s().stepping,
    canPause: () => s().canPause,
    canResume: () => s().canResume,
    canStep: () => s().canStep,
    error: () => s().failure?.message ?? null,
    errorFromServer: () => s().failure?.kind === 'server',
    pause: () => control.pause(),
    resume: () => control.resume(),
    step: (ms: number) => control.step(ms),
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

  /** サーバーの情報（features を変えられる）を読ませ、WebSocket を開く。 */
  async function open(features?: string[]): Promise<{ time: TimeView; socket: FakeSocket }> {
    const loaded = client.loadApp();
    const answer = structuredClone(app) as {
      launcher: { version: { features: string[] } };
    };
    if (features) answer.launcher.version.features = features;
    http.expectOne('api/app').flush(answer);
    await loaded;
    client.connect();
    const socket = fake.last();
    socket.open();
    return { time: view(client), socket };
  }

  /** 例の通知を、条件に合う通知まで（それも含めて）流す。流した数を返す。 */
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

  it('follows the time of a run, and allows only the commands that fit', async () => {
    const { time, socket } = await open();
    expect(time.supported()).toBe(true);
    expect(time.controllable()).toBe(false);

    // 止めた状態で始まった: 再開とステップ送りだけ
    let at = await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    expect(time.simMs()).toBe(0);
    expect(time.paused()).toBe(true);
    expect(time.speed()).toBe('max');
    expect([time.canPause(), time.canResume(), time.canStep()]).toEqual([false, true, true]);

    time.step(25);
    const step = http.expectOne('api/time');
    expect(step.request.method).toBe('POST');
    expect(step.request.body).toEqual({ command: 'step', ms: 25 });
    expect(time.sending()).toBe('step');
    step.flush({ session_id: 1, command: { command: 'step', ms: 25 } });
    // ランチャーの答え（command の事象）が届くまでは、次の命令を送らない（ダブルクリックで 2 回進まない）
    expect(time.sending()).toBe('step');
    expect(time.canStep()).toBe(false);
    // 待っている間に送ろうとしても送らず、そう返す（黙って捨てない）
    expect(time.step(25)).toEqual({ sent: false, reason: 'waiting' });
    http.expectNone('api/time');

    // ランチャーが刻みに切り上げた量と、着いた時刻
    at = await play(socket, 'ws-time', at, timeEvent({ sim_ms: 30, paused: true }));
    expect(time.lastCommand()).toMatchObject({ name: 'step', accepted: true, ms: 30 });
    expect(time.simMs()).toBe(30);
    expect(time.sending()).toBeNull();
    expect(time.canStep()).toBe(true);

    // 動いている: 一時停止だけ。等速では遅れを数える
    at = await play(socket, 'ws-time', at, timeEvent({ paused: false }));
    expect([time.canPause(), time.canResume(), time.canStep()]).toEqual([true, false, false]);
    expect(time.speed()).toBe(1);
    expect(time.paced()).toBe(true);
    expect(time.lateness()).toEqual({ lateSteps: 0, maxLateMs: 0, slippedMs: 0, lateMs: 0 });
    expect(time.behind()).toBe(false);

    // ランチャーが断った命令は、その理由
    at = await play(
      socket,
      'ws-time',
      at,
      (m) => (m['event'] as Record<string, unknown> | undefined)?.['accepted'] === false,
    );
    expect(time.lastCommand()).toMatchObject({ name: 'step', accepted: false });
    expect(time.lastCommand()?.message).toContain('paused');

    // 終わったら、どれも押せない
    await play(socket, 'ws-time', at);
    expect(time.controllable()).toBe(false);
    expect([time.canPause(), time.canResume(), time.canStep()]).toEqual([false, false, false]);
    expect(time.paused()).toBe(false);
  });

  it('takes no command once the simulation is ending, though nobody asked it to stop', async () => {
    const { time, socket } = await open();
    const phase = (name: string) => (m: Record<string, unknown>) =>
      (m['event'] as Record<string, unknown> | undefined)?.['phase'] === name;
    // ws-pass: stop_at_ms に着いて、ランチャーが participant を終わらせている間（結果はまだ）
    let at = await play(socket, 'ws-pass', 0, timeEvent({ sim_ms: 100 }));
    expect(time.controllable()).toBe(true);
    expect(time.canPause()).toBe(true);
    at = await play(socket, 'ws-pass', at, phase('stopping'));
    const session = client.session.session.get();
    expect([session?.phase, session?.stopRequests, session?.result]).toEqual(['stopping', 0, null]);
    expect(time.controllable()).toBe(false);
    expect([time.canPause(), time.canResume(), time.canStep()]).toEqual([false, false, false]);
    await play(socket, 'ws-pass', at);
  });

  it('takes resume and another step during a step, as the launcher does', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    const base = { seq: 1000, t_ms: 0 };
    const send = (seq: number, event: Record<string, unknown>) =>
      socket.send({ type: 'event', session: 1, event: { ...base, seq, ...event } });
    send(1000, {
      event: 'command',
      command: 'step',
      source: 'stdin',
      accepted: true,
      message: null,
      ms: 600,
    });
    send(1001, {
      event: 'time',
      sim_ms: 100,
      speed: 1,
      paused: false,
      actual_speed: null,
      late_steps: 0,
      max_late_ms: 0,
      slipped_ms: 0,
      late_ms: 0,
    });
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.stepping()).toBe(true);
    expect([time.canPause(), time.canResume(), time.canStep()]).toEqual([true, true, true]);
  });

  it('does not call a small lateness within the tolerance late', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    socket.send({
      type: 'event',
      session: 1,
      event: {
        event: 'time',
        seq: 1000,
        t_ms: 0,
        sim_ms: 100,
        speed: 1,
        paused: false,
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

  /** 送って、サーバーの応答（200）まで進める。ランチャーの答えはまだ来ない。 */
  async function sendStep(time: TimeView): Promise<void> {
    expect(time.step(10)).toEqual({ sent: true });
    http.expectOne('api/time').flush({ session_id: 1, command: { command: 'step', ms: 10 } });
    expect(time.sending()).toBe('step');
  }

  /** ws-time の最後の要約（終わった実行）を、変えて写したもの。 */
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
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    await sendStep(time);
    socket.send({ type: 'session', session: lastSummary((s) => (s['last_command'] = null)) });
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBeNull();
  });

  it('stops waiting for an answer when another run has started', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
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
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    await sendStep(time);
    const hello = (seq: number) => ({
      type: 'hello',
      vehicle_sil_gui: 1,
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
    // 送る前の答えだけの要約: まだ待つ
    const before = client.session.session.get()?.lastCommand?.seq ?? -1;
    socket.send(hello(before));
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBe('step');
    // 送った後の答えのある要約: 待つのをやめる
    socket.send(hello(before + 50));
    vi.advanceTimersByTime(BATCH_MS);
    expect(time.sending()).toBeNull();
  });

  it('stops waiting after a while when the launcher does not answer, and says so', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    await sendStep(time);
    expect(time.canResume()).toBe(false);
    vi.advanceTimersByTime(ANSWER_MS);
    expect(time.sending()).toBeNull();
    expect(time.canResume()).toBe(true);
    expect(time.error()).toContain('did not answer the command (step) within 1 s');
    expect(time.errorFromServer()).toBe(false);
  });

  it('says why the server refused a command, for the run it was sent to', async () => {
    const { time, socket } = await open();
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    time.resume();
    http
      .expectOne('api/time')
      .flush(
        { error: { code: 'not-running', message: 'no simulation is running', detail: null } },
        { status: 409, statusText: 'Conflict' },
      );
    expect(time.error()).toBe('no simulation is running');
    expect(time.errorFromServer()).toBe(true);
    expect(time.sending()).toBeNull(); // 送れなかった: 答えは待たない
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
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    await sendStep(time);
    client.close();
    expect(time.sending()).toBeNull();
  });

  it('offers no command to a launcher without time commands', async () => {
    const { time, socket } = await open(['events', 'stdin-commands', 'check-variables']);
    await play(socket, 'ws-time', 0, timeEvent({ paused: true }));
    expect(time.supported()).toBe(false);
    expect(time.controllable()).toBe(false);
    expect(time.canResume()).toBe(false);
  });
});
