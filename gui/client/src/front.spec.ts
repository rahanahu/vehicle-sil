import { BATCH_MS } from './connection';
import { ActionError, Front } from './front-types';
import { connect, createFront, until, watch } from './front';
import { HttpError } from './http';
import { FakeHttp } from './testing/fake-http';
import { FakeSocket, FakeWebSockets } from './testing/fake-web-socket';
import { httpExamples } from './testing/protocol-examples';

// The front door against a fake server: what `status`, `clock` and `controls` say for what the launcher's `state` and
// the GUI server's `allowed` say, and how the Promise operations end. Server notifications come from a fake WebSocket
// (the batches are delivered by advancing the fake clock), HTTP is answered by hand.

const ANSWER_MS = 1000;

const ok = { ok: true, reason: null, message: null, caution: null };
const refusal = (reason: string, message: string) => ({
  ok: false,
  reason,
  message,
  caution: null,
});

/** The launcher's `allowed`, with some entries changed. */
const launcherAllowed = (more: Record<string, object> = {}) => ({
  pause: ok,
  resume: refusal('not-paused', 'resume works while the simulation is paused'),
  step: {
    ...refusal('not-paused', 'step works while the simulation is paused'),
    min_ms: 10,
    max_ms: 5000,
  },
  speed: { ...ok, min: 0.5, max: 100 },
  run_until: ok,
  stop: ok,
  write: ok,
  ...more,
});

/** A `state` event of the launcher. */
const stateEvent = (
  rev: number,
  more: Record<string, unknown> = {},
  allowed: Record<string, object> = {},
) => ({
  event: 'state',
  seq: 10 + rev,
  t_ms: 100 * rev,
  rev,
  phase: 'running',
  paused: false,
  paused_at_ms: null,
  holding: false,
  speed: 1,
  pacing: 'on-time',
  stop: { requests: 0, by: null },
  scenario: false,
  allowed: launcherAllowed(allowed),
  outside: [],
  ...more,
});

/** A state that says paused; `at`: where (`paused_at_ms`; null: the simulation has not come to a stand yet). */
const pausedState = (rev: number, at: number | null = null) =>
  stateEvent(
    rev,
    { paused: true, paused_at_ms: at },
    { pause: ok, resume: ok, step: { ...ok, min_ms: 10, max_ms: 5000 } },
  );

/** A `time` event: where virtual time is (whether it stands still is the state's to say). */
const timeEvent = (seq: number, simMs: number) => ({
  event: 'time',
  seq,
  t_ms: seq,
  sim_ms: simMs,
  actual_speed: 1,
});

const commandEvent = (
  seq: number,
  command: string,
  id: number | null,
  more: Record<string, unknown> = {},
) => ({
  event: 'command',
  seq,
  t_ms: seq,
  command,
  source: 'stdin',
  accepted: true,
  message: null,
  reason: null,
  ms: null,
  id,
  ...more,
});

const serverAllowed = (_rev: number, start: object, stop: object = ok, force: object = stop) => ({
  start,
  stop,
  force,
  save: { ok: true, reason: null, message: null, caution: null },
});
const startOk = { ok: true, reason: null, message: null };
const notRunning = {
  ok: false,
  reason: 'not-running',
  message: 'no simulation is running: there is nothing to stop',
};
const alreadyRunning = {
  ok: false,
  reason: 'already-running',
  message: 'a simulation is running: stop it before starting another',
};

const summary = (more: Record<string, unknown> = {}, id: string | number = '1') => ({
  id: String(id),
  file: 'sim.yaml',
  state: 'running',
  launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
  stop: 'none',
  phase: 'running',
  launcher_state: null,
  allowed: serverAllowed(3, alreadyRunning),
  ...more,
});

/** Events get a rising seq (an event whose seq is not after the last one is dropped). */
let lastSeq = 0;
const event = (e: unknown, session: string | number = '1') => ({
  type: 'event',
  run_id: String(session),
  event: { ...(e as object), seq: ++lastSeq },
});

interface Rig {
  front: Front;
  http: FakeHttp;
  socket: FakeSocket;
  /** Delivers notifications as one batch. */
  deliver(...messages: unknown[]): Promise<void>;
  /** Takes the one unanswered api/time and returns its body. */
  timeBody(): { command: string; id: number; [key: string]: unknown };
  turn(): Promise<void>;
}

describe('the front door', () => {
  let rigs: Rig[];
  let app: unknown;

  beforeAll(async () => {
    app = (await httpExamples()).find((e) => e.name === 'app')?.response;
  });

  beforeEach(() => {
    vi.useFakeTimers();
    rigs = [];
    lastSeq = 0;
  });

  afterEach(() => {
    for (const rig of rigs) rig.front.close();
    vi.useRealTimers();
  });

  it('createFront makes the front at once, without connecting or asking the server', () => {
    const http = new FakeHttp();
    const fake = new FakeWebSockets();
    const front = createFront({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: fake.factory,
    });
    try {
      expect(http.requests).toHaveLength(0);
      expect(fake.sockets).toHaveLength(0);
      // Before the app answered, a start without a file cannot be sent
      expect(front.controls.get().start.ok).toBe(false);
      front.low.connect();
      expect(fake.sockets).toHaveLength(1);
    } finally {
      front.close();
    }
  });

  async function open(appAnswer: unknown = app): Promise<Rig> {
    const fake = new FakeWebSockets();
    const http = new FakeHttp();
    const promise = connect({
      baseUrl: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: fake.factory,
      answerTimeoutMs: ANSWER_MS,
      onError: (error) => {
        throw error;
      },
    });
    http.expectOne('api/app').flush(appAnswer);
    const front = await promise;
    const socket = fake.last();
    socket.open();
    const rig: Rig = {
      front,
      http,
      socket,
      deliver: async (...messages) => {
        for (const message of messages) socket.send(message);
        await vi.advanceTimersByTimeAsync(BATCH_MS);
      },
      timeBody: () => http.expectOne('api/time').request.body as ReturnType<Rig['timeBody']>,
      turn: async () => {
        await vi.advanceTimersByTimeAsync(0);
      },
    };
    rigs.push(rig);
    return rig;
  }

  /** A run that is going: connected, in sync, the launcher's state is `state`. */
  async function running(state: unknown = stateEvent(1)): Promise<Rig> {
    const rig = await open();
    await rig.deliver(
      {
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'x',
        session: summary(),
        allowed: serverAllowed(3, alreadyRunning),
        replay: null,
      },
      event(state),
      event(timeEvent(30, 500)),
      { type: 'synced', run_id: '1', last_seq: 40 },
    );
    return rig;
  }

  describe('what it shows', () => {
    it('says the run is running, with its clock, and takes each control from the launcher', async () => {
      const { front } = await running();
      expect(front.status.get()).toEqual({
        link: 'online',
        run: 'running',
        file: 'sim.yaml',
        result: null,
        problem: null,
      });
      expect(front.clock.get()).toEqual({
        sim_ms: 500,
        speed: 1,
        actual_speed: 1,
        pacing: 'on-time',
      });
      const c = front.controls.get();
      expect(c.pause).toEqual({ ok: true, reason: null, caution: null });
      expect(c.resume.reason).toEqual({
        code: 'not-paused',
        message: 'resume works while the simulation is paused',
      });
      expect(c.step).toMatchObject({ ok: false, min_ms: 10, max_ms: 5000 });
      expect(c.speed).toMatchObject({ ok: true, min: 0.5, max: 100, fastest: true });
      expect(c.start.reason).toEqual({ code: 'already-running', message: alreadyRunning.message });
      expect(c.stop.ok).toBe(true);
      expect(c.force_stop).toMatchObject({
        ok: false,
        reason: { code: 'not-stopping' },
      });
    });

    it('makes pause not ok while paused, because it would change nothing, and resume ok', async () => {
      const { front } = await running(pausedState(1));
      expect(front.status.get().run).toBe('paused');
      const c = front.controls.get();
      expect(c.pause).toMatchObject({ ok: false, reason: { code: 'paused' } });
      expect(c.resume.ok).toBe(true);
      expect(c.step.ok).toBe(true);
    });

    it('reports a start paused during the waiting phase as paused', async () => {
      const { front } = await running(
        stateEvent(
          1,
          { phase: 'waiting', paused: true, paused_at_ms: 0, holding: true },
          { pause: ok, resume: ok, step: { ...ok, min_ms: 10, max_ms: 5000 } },
        ),
      );
      expect(front.status.get().run).toBe('paused');
      expect(front.controls.get().resume.ok).toBe(true);
      expect(front.controls.get().step.ok).toBe(true);
    });

    it('keeps waiting while participants have not reached the initial hold', async () => {
      const { front } = await running(
        stateEvent(1, { phase: 'waiting', paused: true, holding: false }, {}),
      );
      expect(front.status.get().run).toBe('waiting');
    });

    it('passes the launcher reason and its caution on as they are', async () => {
      const { front } = await running(
        stateEvent(
          1,
          {},
          {
            speed: {
              ...ok,
              min: 1,
              max: 1,
              caution: { code: 'outside-real-time', message: 'it keeps to real time' },
            },
          },
        ),
      );
      const c = front.controls.get();
      expect(c.speed.caution).toEqual({
        code: 'outside-real-time',
        message: 'it keeps to real time',
      });
      // A range of one speed does not take the fastest (the launcher's promise)
      expect(c.speed).toMatchObject({ min: 1, max: 1, fastest: false });
    });

    it('accepts the fastest speed while the range has more than one speed', async () => {
      const { front } = await running();
      expect(front.controls.get().speed).toMatchObject({ min: 0.5, max: 100, fastest: true });
    });

    it('keeps the same object while nothing changed', async () => {
      const rig = await running();
      const before = [rig.front.status.get(), rig.front.controls.get(), rig.front.clock.get()];
      await rig.deliver(event(timeEvent(31, 500)));
      expect([rig.front.status.get(), rig.front.controls.get(), rig.front.clock.get()]).toEqual(
        before,
      );
      expect(rig.front.status.get()).toBe(before[0]);
      expect(rig.front.controls.get()).toBe(before[1]);
      expect(rig.front.clock.get()).toBe(before[2]);
    });

    it('says there is no run when there is none, and why each time operation is not possible', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      const { front } = rig;
      expect(front.status.get()).toMatchObject({
        link: 'online',
        run: 'none',
        result: null,
        problem: null,
      });
      const c = front.controls.get();
      expect(c.start.ok).toBe(true);
      expect(c.pause.reason?.code).toBe('not-running');
      expect(c.stop.reason?.code).toBe('not-running');
    });

    it("turns start off with the server's own words when another page has started a run (starting)", async () => {
      const rig = await open();
      const starting = {
        ok: false,
        reason: 'starting',
        message:
          'a run is starting: wait until it has started, then stop it before starting another',
      };
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      expect(rig.front.controls.get().start.ok).toBe(true);
      await rig.deliver({ type: 'allowed', run_id: null, allowed: serverAllowed(2, starting) });
      expect(rig.front.controls.get().start).toEqual({
        ok: false,
        reason: { code: 'starting', message: starting.message },
        caution: null,
      });
      expect(rig.front.status.get().run).toBe('starting');
      // Another page's start has no session identity until its response or notification.
      expect(rig.front.controls.get().stop.reason?.code).toBe('starting');
      expect(rig.front.controls.get().force_stop.reason?.code).toBe('starting');
      await rig.deliver({
        type: 'allowed',
        run_id: null,
        allowed: serverAllowed(3, startOk, notRunning),
      });
      expect(rig.front.controls.get().start.ok).toBe(true);
      expect(rig.front.status.get().run).toBe('none');
    });

    it('does not let an older state of the launcher replace a newer one', async () => {
      const rig = await running(pausedState(5));
      await rig.deliver(event(stateEvent(3)));
      expect(rig.front.status.get().run).toBe('paused');
      expect(rig.front.controls.get().resume.ok).toBe(true);
    });

    it('does not let an older allowed stream_seq replace a newer one after synced', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          stream_seq: 5,
          session: null,
          allowed: serverAllowed(5, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', stream_seq: 5, run_id: null, last_seq: null },
        { type: 'allowed', stream_seq: 4, run_id: null, allowed: serverAllowed(4, alreadyRunning) },
      );
      expect(rig.front.controls.get().start.ok).toBe(true);
    });

    it('calls the run stopping, with no result, while the ended phase is in but the result is not', async () => {
      const rig = await running();
      await rig.deliver(event(stateEvent(2, { phase: 'ended' })));
      expect(rig.front.status.get()).toMatchObject({ run: 'stopping', result: null });
    });

    it('claims neither pause nor resume to change nothing while a step is on its way', async () => {
      const rig = await running(pausedState(1));
      const controls = () => {
        const { pause, resume } = rig.front.controls.get();
        return [pause.reason?.code ?? null, resume.reason?.code ?? null];
      };
      expect(controls()).toEqual(['paused', null]);
      // The step is accepted (to 530 ms). The state from before the command still says paused: not believed
      await rig.deliver(event(commandEvent(60, 'step', null, { ms: 30, at_ms: 530 })));
      expect(controls()).toEqual([null, null]);
      // The command's state says not paused, and the launcher still takes a resume: not called "not paused"
      await rig.deliver(event(stateEvent(2, { paused: false }, { pause: ok, resume: ok })));
      expect(controls()).toEqual([null, null]);
      // Time is there, but the state that says so has not come (the launcher writes it right after)
      await rig.deliver(event(timeEvent(61, 530)));
      expect(controls()).toEqual([null, null]);
      // Arrived: time at the command's at_ms, and the latest state says paused
      await rig.deliver(event(pausedState(3, 530)));
      expect(controls()).toEqual(['paused', null]);
    });

    it('takes the arrival from a state that says where it stands, before the time event of that time', async () => {
      const rig = await running(pausedState(1, 500));
      // The state from before the command stands at 500: not there. Then the simulation was there before the
      // command's own state was put together: that state says paused at 530 already, and no time event has said so
      await rig.deliver(event(commandEvent(60, 'step', null, { ms: 30, at_ms: 530 })));
      expect(rig.front.controls.get().pause.ok).toBe(true);
      await rig.deliver(event(pausedState(2, 530)));
      expect(rig.front.controls.get().pause.reason?.code).toBe('paused');
      // The clock is where the state says it stands, though the last time event is older
      expect(rig.front.clock.get().sim_ms).toBe(530);
      await rig.deliver(event(timeEvent(61, 530)));
      expect(rig.front.clock.get().sim_ms).toBe(530);
    });

    it('does not take a state that says paused without saying where for the arrival', async () => {
      const rig = await running(pausedState(1));
      await rig.deliver(
        event(commandEvent(60, 'step', null, { ms: 30, at_ms: 530 })),
        event(pausedState(2)),
      );
      expect(rig.front.controls.get().pause.ok).toBe(true);
      // A time event at the target is not the arrival either: the state must say where it stands
      await rig.deliver(event(timeEvent(61, 530)));
      expect(rig.front.controls.get().pause.ok).toBe(true);
      await rig.deliver(event(pausedState(3, 530)));
      expect(rig.front.controls.get().pause.reason?.code).toBe('paused');
    });

    it('goes back to the state once another command ends the step on its way', async () => {
      const rig = await running(pausedState(1));
      await rig.deliver(
        event(commandEvent(60, 'step', null, { ms: 1000, at_ms: 1500 })),
        event(stateEvent(2, { paused: false }, { pause: ok, resume: ok })),
        event(commandEvent(62, 'resume', null)),
      );
      expect(rig.front.controls.get().resume.reason?.code).toBe('not-paused');
    });

    it('lets the run be stopped while the launcher runs on after its result, as the server says', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'passed',
            end: 'completed',
            ok: true,
            message: 'The run reached its end without errors',
            interrupted_by: null,
            errors: 0,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [],
          },
        }),
      );
      expect(rig.front.status.get().run).toBe('ended');
      expect(rig.front.controls.get().stop.ok).toBe(true);
    });

    it('keeps force_stop available after a first stop while a cold launcher runs on after its result', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'passed',
            end: 'completed',
            ok: true,
            message: 'The run reached its end without errors',
            interrupted_by: null,
            errors: 0,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [],
          },
        }),
      );
      expect(rig.front.status.get().run).toBe('ended');
      expect(rig.front.controls.get().stop.ok).toBe(true);
      expect(rig.front.controls.get().force_stop).toMatchObject({
        ok: false,
        reason: { code: 'not-stopping' },
      });

      const firstStop = rig.front.run.stop();
      const firstRequest = rig.http.expectOne('api/stop-session');
      expect(firstRequest.request.body).toEqual({ force: false, run_id: '1', instance: 'x' });
      firstRequest.flush({ instance: 'x', run_id: '1' });
      await firstStop;

      const alreadyStopping = refusal(
        'already-stopping',
        'a stop was already asked for: wait for the run to end, or stop it without waiting (force)',
      );
      await rig.deliver({
        type: 'session',
        session: summary({
          state: 'running',
          launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
          stop: 'requested',
          allowed: serverAllowed(4, alreadyRunning, alreadyStopping, ok),
        }),
      });
      expect(rig.front.status.get().run).toBe('ended');
      expect(rig.front.controls.get().stop).toMatchObject({
        ok: false,
        reason: { code: 'already-stopping' },
      });
      expect(rig.front.controls.get().force_stop).toEqual({
        ok: true,
        reason: null,
        caution: null,
      });

      const force = rig.front.run.forceStop();
      const forceRequest = rig.http.expectOne('api/stop-session');
      expect(forceRequest.request.body).toEqual({ force: true, run_id: '1', instance: 'x' });
      forceRequest.flush({ instance: 'x', run_id: '1' });
      await expect(force).resolves.toBeUndefined();

      await rig.deliver({
        type: 'session',
        session: summary({
          state: 'running',
          launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
          stop: 'forced',
          allowed: serverAllowed(
            5,
            alreadyRunning,
            alreadyStopping,
            refusal('already-forced', 'the stop was already forced: wait for the run to end'),
          ),
        }),
      });
      expect(rig.front.controls.get().force_stop).toMatchObject({
        ok: false,
        reason: { code: 'already-forced' },
      });

      await rig.deliver({
        type: 'session',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
          stop: 'forced',
          allowed: serverAllowed(6, startOk, notRunning, notRunning),
        }),
      });
      expect(rig.front.controls.get().stop.reason?.code).toBe('not-running');
      expect(rig.front.controls.get().force_stop.reason?.code).toBe('not-running');
    });

    it('offers force_stop and reports stopping after an external first stop while live', async () => {
      const rig = await running();
      const alreadyStopping = refusal(
        'already-stopping',
        'a stop was already asked for: wait for the run to end, or stop it without waiting (force)',
      );
      await rig.deliver(event(stateEvent(2, { stop: { requests: 1, by: 'control_server' } })), {
        type: 'allowed',
        run_id: '1',
        allowed: serverAllowed(4, alreadyRunning, alreadyStopping, ok),
      });
      expect(rig.front.status.get().run).toBe('stopping');
      expect(rig.front.controls.get().stop).toMatchObject({
        ok: false,
        reason: { code: 'already-stopping' },
      });
      expect(rig.front.controls.get().force_stop).toEqual({
        ok: true,
        reason: null,
        caution: null,
      });

      const force = rig.front.run.forceStop();
      const forceRequest = rig.http.expectOne('api/stop-session');
      expect(forceRequest.request.body).toEqual({ force: true, run_id: '1', instance: 'x' });
      forceRequest.flush({ instance: 'x', run_id: '1' });
      await expect(force).resolves.toBeUndefined();
    });

    it('offers force_stop after an external first stop on a post-result cold launcher, and keeps hot drain not-running', async () => {
      const rig = await running();
      const alreadyStopping = refusal(
        'already-stopping',
        'a stop was already asked for: wait for the run to end, or stop it without waiting (force)',
      );
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'passed',
            end: 'completed',
            ok: true,
            message: 'The run reached its end without errors',
            interrupted_by: null,
            errors: 0,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [],
          },
        }),
        event(stateEvent(2, { phase: 'ended', stop: { requests: 1, by: 'control_server' } })),
        {
          type: 'allowed',
          run_id: '1',
          allowed: serverAllowed(4, alreadyRunning, alreadyStopping, ok),
        },
      );
      expect(rig.front.status.get().run).toBe('ended');
      expect(rig.front.controls.get().stop).toMatchObject({
        ok: false,
        reason: { code: 'already-stopping' },
      });
      expect(rig.front.controls.get().force_stop).toEqual({
        ok: true,
        reason: null,
        caution: null,
      });

      const force = rig.front.run.forceStop();
      const forceRequest = rig.http.expectOne('api/stop-session');
      expect(forceRequest.request.body).toEqual({ force: true, run_id: '1', instance: 'x' });
      forceRequest.flush({ instance: 'x', run_id: '1' });
      await expect(force).resolves.toBeUndefined();

      // When the server marks both stop and force as not-running (e.g. hot post-result drain), both stay not-running.
      await rig.deliver({
        type: 'allowed',
        run_id: '1',
        allowed: serverAllowed(5, alreadyRunning, notRunning, notRunning),
      });
      expect(rig.front.controls.get().stop.reason?.code).toBe('not-running');
      expect(rig.front.controls.get().force_stop.reason?.code).toBe('not-running');
    });

    it('passes on what the launcher says of the result, and a stop by the operator is not a failure', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'stopped',
            end: 'stopped',
            ok: true,
            message: 'The run was stopped by a stop request without errors',
            interrupted_by: 'control server stop',
            errors: 0,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [],
          },
        }),
        {
          type: 'session',
          session: summary({
            state: 'ended',
            launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
            allowed: serverAllowed(4, startOk, notRunning),
          }),
        },
      );
      const status = rig.front.status.get();
      expect(status.run).toBe('ended');
      expect(status.result).toEqual({
        end: 'stopped',
        ok: true,
        scenario: null,
        message: 'The run was stopped by a stop request without errors',
      });
      expect(status.problem).toBeNull();
      expect(rig.front.controls.get().start.ok).toBe(true);
    });

    it('shows a scenario that a stop cut short as the problem, in the words of the launcher', async () => {
      const rig = await running();
      const message =
        'The run was stopped by a stop request before the scenario reached its end, so the scenario has no verdict';
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'incomplete',
            end: 'stopped',
            ok: false,
            message,
            interrupted_by: 'stop command',
            errors: 0,
            warnings: 0,
            notes: 2,
            participants: [],
            findings: [],
            scenario: {
              file: 's.yaml',
              name: null,
              status: 'incomplete',
              over_at_ms: null,
              steps: [],
            },
          },
        }),
      );
      const status = rig.front.status.get();
      expect(status.result).toEqual({ end: 'stopped', ok: false, scenario: 'incomplete', message });
      expect(status.problem).toEqual({ code: 'run-failed', message });
    });

    it('has no end for a result of a run in which nothing was started', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'check_failed',
            end: null,
            ok: false,
            message: 'Nothing was started, because the check failed: no such file',
            interrupted_by: null,
            errors: 1,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [],
          },
        }),
      );
      const status = rig.front.status.get();
      expect(status.run).toBe('ended');
      expect(status.result).toMatchObject({ end: null, ok: false });
      expect(status.problem?.message).toBe(
        'Nothing was started, because the check failed: no such file',
      );
    });

    it('shows a failed run as the problem, and the launcher dying without a result as crashed', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 2,
            result: 'failed',
            end: 'failed',
            ok: false,
            message: 'The run failed: Engine failed',
            interrupted_by: null,
            errors: 1,
            warnings: 0,
            notes: 0,
            participants: [],
            findings: [{ severity: 'error', code: 'participant-failed', message: 'Engine failed' }],
          },
        }),
        {
          type: 'session',
          session: summary({
            state: 'ended',
            launcher: { pid: 1, running: false, exit_code: 1, signal: null, silent_s: null },
          }),
        },
      );
      expect(rig.front.status.get().problem).toEqual({
        code: 'run-failed',
        message: 'The run failed: Engine failed',
      });
      const crashed = await running();
      await crashed.deliver({
        type: 'session',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: null, signal: 9, silent_s: null },
        }),
      });
      expect(crashed.front.status.get()).toMatchObject({
        run: 'crashed',
        result: { end: 'crashed', ok: false },
        problem: { code: 'launcher-crashed' },
      });
    });

    it('puts the connection first in the problem, and offline turns the time controls off but not stop', async () => {
      const rig = await running();
      rig.socket.fail();
      await rig.turn();
      const status = rig.front.status.get();
      expect(status.link).toBe('offline');
      expect(status.problem?.code).toBe('offline');
      const c = rig.front.controls.get();
      expect(c.pause.reason?.code).toBe('offline');
      expect(c.start.reason?.code).toBe('offline');
      expect(c.stop.ok).toBe(true);
    });

    it('turns every operation off with the token refused, with a problem that does not go away', async () => {
      const rig = await open();
      await rig.deliver({
        type: 'error',
        run_id: null,
        code: 'unauthorized',
        message: 'the token is wrong',
        detail: null,
      });
      expect(rig.front.status.get()).toMatchObject({
        link: 'rejected',
        problem: { code: 'rejected' },
      });
      await expect(rig.front.time.pause()).rejects.toMatchObject({
        code: 'rejected',
        outcome: 'refused',
      });
      rig.http.expectNone('api/time');
    });

    it('gives the scenario of the run', async () => {
      const rig = await running();
      expect(rig.front.scenario.get()).toBeNull();
      await rig.deliver({
        type: 'session',
        session: summary({
          scenario: {
            file: 's.yaml',
            name: 'ramp',
            step_count: 1,
            steps: [],
            steps_omitted: 0,
            status: null,
            over_at_ms: null,
            timeline_url: null,
          },
        }),
      });
      expect(rig.front.scenario.get()).toMatchObject({
        file: 's.yaml',
        name: 'ramp',
        status: null,
        steps: [],
      });
    });
  });

  describe('the problem', () => {
    it('shows the newer of a crash and the refusal that came after it', async () => {
      const rig = await running();
      await rig.deliver({
        type: 'session',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: null, signal: 9, silent_s: null },
          allowed: serverAllowed(4, startOk, notRunning),
        }),
      });
      expect(rig.front.status.get().problem?.code).toBe('launcher-crashed');
      const start = rig.front.run.start(undefined, { file: 'sim.yaml' });
      rig.http.expectOne('api/run').flush(
        {
          error: {
            code: 'check-failed',
            message: 'the check of sim.yaml found 1 error',
            detail: { findings: [{ severity: 'error', code: 'x', message: 'bad' }] },
          },
        },
        { status: 422, statusText: 'Unprocessable' },
      );
      const error = await start.catch((e: unknown) => e as ActionError);
      expect(error).toBeInstanceOf(ActionError);
      expect(error).toMatchObject({ code: 'check-failed', outcome: 'refused' });
      expect((error as ActionError).findings).toHaveLength(1);
      expect(rig.front.status.get().problem).toEqual({
        code: 'check-failed',
        message: 'the check of sim.yaml found 1 error',
      });
    });

    it('drops a refusal when the run changes state, and at the next action', async () => {
      const rig = await running();
      const rejected = rig.front.time.step(5).catch((e: unknown) => e as ActionError);
      const id = rig.timeBody().id;
      await rig.deliver(
        event(
          commandEvent(60, 'step', id, {
            accepted: false,
            reason: 'not-paused',
            message: 'step works while the simulation is paused',
          }),
        ),
      );
      expect(await rejected).toMatchObject({ code: 'not-paused', outcome: 'refused' });
      expect(rig.front.status.get().problem).toEqual({
        code: 'not-paused',
        message: 'step works while the simulation is paused',
      });
      await rig.deliver(event(pausedState(2)));
      expect(rig.front.status.get().problem).toBeNull();
    });
  });

  describe('the time operations', () => {
    it('does not send a step from a rendered view of another run', async () => {
      const rig = await running(pausedState(1));
      const shown = rig.front.low.session.currentTarget();
      await rig.deliver({ type: 'session', session: summary({}, 2) });
      await expect(rig.front.time.step(10, shown)).rejects.toMatchObject({
        code: 'session-changed',
        outcome: 'refused',
      });
      rig.http.expectNone('api/time');
    });

    it('ties the answer to the command by its id, and ignores the same name with another id', async () => {
      const rig = await running();
      let done = false;
      const pause = rig.front.time.pause().then(() => (done = true));
      const body = rig.timeBody();
      expect(body).toMatchObject({ command: 'pause', run_id: '1', instance: 'x' });
      expect(Number.isSafeInteger(body.id)).toBe(true);
      // Another page's pause (another id) and a stdin line (no id) are not our answer
      await rig.deliver(
        event(commandEvent(60, 'pause', body.id + 1)),
        event(commandEvent(61, 'pause', null)),
      );
      expect(done).toBe(false);
      await rig.deliver(event(commandEvent(62, 'pause', body.id)));
      await pause;
      expect(done).toBe(true);
    });

    it('delivers an acknowledgement buffered before the socket closes', async () => {
      const rig = await running();
      const pause = rig.front.time.pause();
      const id = rig.timeBody().id;
      rig.socket.send(event(commandEvent(62, 'pause', id)));
      rig.socket.fail();
      await expect(pause).resolves.toBeUndefined();
    });

    it('delivers a valid acknowledgement before a malformed message forces a reconnect', async () => {
      const rig = await running();
      const pause = rig.front.time.pause();
      const id = rig.timeBody().id;
      await rig.deliver(event(commandEvent(62, 'pause', id)), {
        type: 'session',
        session: { id: '' },
      });
      await expect(pause).resolves.toBeUndefined();
    });

    it('sends ids that differ between commands', async () => {
      const rig = await running();
      void rig.front.time.setSpeed(2).catch(() => undefined);
      const first = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'speed', first)));
      void rig.front.time.setSpeed('max').catch(() => undefined);
      expect(rig.timeBody()).toMatchObject({ command: 'speed', value: 'max' });
    });

    it("rejects with the launcher's reason and sentence when it does not accept", async () => {
      const rig = await running();
      const resume = rig.front.time.setSpeed(1000).catch((e: unknown) => e as ActionError);
      const id = rig.timeBody().id;
      await rig.deliver(
        event(
          commandEvent(60, 'speed', id, {
            accepted: false,
            reason: 'out-of-range',
            message: 'the speed is out of range',
          }),
        ),
      );
      expect(await resume).toMatchObject({
        code: 'out-of-range',
        message: 'the speed is out of range',
        outcome: 'refused',
      });
    });

    it('gives up after the answer time-out with no-answer, which may have been done', async () => {
      const rig = await running();
      const pause = rig.front.time.pause().catch((e: unknown) => e as ActionError);
      rig.timeBody();
      await vi.advanceTimersByTimeAsync(ANSWER_MS);
      expect(await pause).toMatchObject({ code: 'no-answer', outcome: 'unknown' });
      // the control is free again
      expect(rig.front.controls.get().pause.ok).toBe(true);
    });

    it('refuses the same operation while its answer is pending, and shows it as busy', async () => {
      const rig = await running();
      void rig.front.time.pause().catch(() => undefined);
      const id = rig.timeBody().id;
      expect(rig.front.controls.get().pause.reason?.code).toBe('busy');
      await expect(rig.front.time.pause()).rejects.toMatchObject({
        code: 'busy',
        outcome: 'refused',
      });
      rig.http.expectNone('api/time');
      await rig.deliver(event(commandEvent(60, 'pause', id)));
      expect(rig.front.controls.get().pause.reason?.code).not.toBe('busy');
    });

    it('does not send a pause when it is paused already, nor a resume when it is running', async () => {
      const paused = await running(pausedState(1));
      await paused.front.time.pause();
      paused.http.expectNone('api/time');
      const going = await running();
      await going.front.time.resume();
      going.http.expectNone('api/time');
    });

    it('refuses a time command when there is no identified run', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      expect(rig.front.controls.get().step.ok).toBe(false);
      await expect(rig.front.time.step(10)).rejects.toMatchObject({
        code: 'not-running',
        outcome: 'refused',
      });
      rig.http.expectNone('api/time');
    });

    it('resolves a step only when time has arrived and the run is paused again', async () => {
      const rig = await running(pausedState(1));
      let done = false;
      const step = rig.front.time.step(25).then(() => (done = true));
      const id = rig.timeBody().id;
      // Accepted; the launcher rounded 25 up to 30: the target is 500 + 30
      await rig.deliver(
        event(commandEvent(60, 'step', id, { ms: 30 })),
        event(stateEvent(2, { paused: false })),
      );
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(61, 520)));
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(62, 530)));
      expect(done).toBe(false);
      await rig.deliver(event(pausedState(3, 530)));
      await step;
      expect(done).toBe(true);
      expect(rig.front.clock.get().sim_ms).toBe(530);
    });

    it('resolves a step when the state says that it stands at the target, without a time event', async () => {
      const rig = await running(pausedState(1, 500));
      let done = false;
      const step = rig.front.time.step(25).then(() => (done = true));
      const id = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'step', id, { ms: 30, at_ms: 530 })));
      expect(done).toBe(false); // the state from before the command stands at 500
      await rig.deliver(event(pausedState(2, 530)));
      await step;
      expect(done).toBe(true);
      expect(rig.front.clock.get().sim_ms).toBe(530);
    });

    it("rejects a step that another page's resume interrupted, as interrupted and unknown", async () => {
      const rig = await running(pausedState(1));
      const step = rig.front.time.step(100).catch((e: unknown) => e as ActionError);
      const id = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'step', id, { ms: 100 })));
      await rig.deliver(event(commandEvent(61, 'resume', 9)));
      expect(await step).toMatchObject({ code: 'interrupted', outcome: 'unknown' });
    });

    it('rejects a step that a stop of the run overtook, and one whose run ended', async () => {
      const rig = await running(pausedState(1));
      const step = rig.front.time.step(100).catch((e: unknown) => e as ActionError);
      const id = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'step', id, { ms: 100 })));
      await rig.deliver({ type: 'session', session: summary({ stop: 'requested' }) });
      expect(await step).toMatchObject({ code: 'interrupted', outcome: 'unknown' });
      expect(rig.front.status.get().problem).toBeNull();

      const other = await running();
      const pause = other.front.time.pause().catch((e: unknown) => e as ActionError);
      other.timeBody();
      await other.deliver({
        type: 'session',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
        }),
      });
      expect(await pause).toMatchObject({ code: 'not-running', outcome: 'unknown' });
    });

    it('does not end a step on its way because another step is sent meanwhile', async () => {
      const rig = await running(pausedState(1));
      let done = false;
      const first = rig.front.time.step(100).then(() => (done = true));
      const id1 = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'step', id1, { ms: 100, at_ms: 600 })));
      void rig.front.time.step(50).catch(() => undefined);
      const id2 = rig.timeBody().id;
      await rig.deliver(event(commandEvent(61, 'step', id2, { ms: 50, at_ms: 650 })));
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(62, 650)), event(pausedState(3, 650)));
      await first;
      expect(done).toBe(true);
    });

    it('uses the at_ms of the command event as the target: a step right after a pause waits for its own time', async () => {
      const rig = await running(pausedState(1));
      let done = false;
      const step = rig.front.time.step(10).then(() => (done = true));
      const id = rig.timeBody().id;
      // the launcher was still stopping from the pause: the last time seen (500) is not where the step starts
      await rig.deliver(
        event(timeEvent(60, 540)),
        event(commandEvent(61, 'step', id, { ms: 10, at_ms: 550 })),
      );
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(62, 550)), event(pausedState(3, 550)));
      await step;
      expect(done).toBe(true);
    });

    it("does not show the end of a step by this page's own pause or by a stop as the problem, but another sender's resume", async () => {
      const rig = await running();
      const own = rig.front.time.step(100).catch((e: unknown) => e as ActionError);
      const idStep = rig.timeBody().id;
      await rig.deliver(event(commandEvent(60, 'step', idStep, { ms: 100, at_ms: 600 })));
      void rig.front.time.pause().catch(() => undefined);
      const idPause = rig.timeBody().id;
      await rig.deliver(event(commandEvent(61, 'pause', idPause)));
      expect(await own).toMatchObject({ code: 'interrupted' });
      expect(rig.front.status.get().problem).toBeNull();

      const other = await running(pausedState(1));
      const step = other.front.time.step(100).catch((e: unknown) => e as ActionError);
      const id = other.timeBody().id;
      await other.deliver(event(commandEvent(60, 'step', id, { ms: 100, at_ms: 600 })));
      await other.deliver(event(commandEvent(61, 'resume', 9)));
      expect(await step).toMatchObject({ code: 'interrupted' });
      expect(other.front.status.get().problem?.code).toBe('interrupted');
    });

    it('sends run_until with its time, and takes the time as the target', async () => {
      const rig = await running(pausedState(1));
      let done = false;
      const until = rig.front.time.runUntil(900).then(() => (done = true));
      const body = rig.timeBody();
      expect(body).toMatchObject({ command: 'run_until', at_ms: 900 });
      await rig.deliver(
        event(commandEvent(60, 'run_until', body.id)),
        event(stateEvent(2, { paused: false })),
      );
      await rig.deliver(event(timeEvent(61, 900)), event(pausedState(3, 900)));
      await until;
      expect(done).toBe(true);
    });

    it('waits for the state to say where it stands after a run_until to a time already passed', async () => {
      const rig = await running(pausedState(1, 500));
      await rig.deliver(event(timeEvent(50, 1000)));
      let done = false;
      const until = rig.front.time.runUntil(900).then(() => (done = true));
      const body = rig.timeBody();
      await rig.deliver(
        event(commandEvent(60, 'run_until', body.id)),
        event(stateEvent(2, { paused: false })),
      );
      // At the maximum speed it is paused but not yet standing at a time; the last time event (1000) is old
      await rig.deliver(event(pausedState(3, null)));
      expect(done).toBe(false);
      await rig.deliver(event(pausedState(4, 1010)));
      await until;
      expect(done).toBe(true);
    });

    it('says the time operations are unsupported until the launcher has said what it accepts', async () => {
      const rig = await open();
      // The start of a run: the launcher runs, its first state has not come yet
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: summary(),
          allowed: serverAllowed(3, alreadyRunning),
          replay: null,
        },
        { type: 'synced', run_id: '1', last_seq: 40 },
      );
      const notYet = {
        ok: false,
        reason: { code: 'unsupported', message: 'the launcher has not said yet what it accepts' },
      };
      expect(rig.front.controls.get().pause).toMatchObject(notYet);
      expect(rig.front.controls.get().run_until).toMatchObject(notYet);
      await rig.deliver(event(stateEvent(1)));
      expect(rig.front.controls.get().pause).toMatchObject({ ok: true, reason: null });
      expect(rig.front.controls.get().run_until).toMatchObject({ ok: true, reason: null });
    });

    it('refuses a second start while the first is on its way, and a start with the token refused', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      void rig.front.run.start(undefined, { file: 'sim.yaml' }).catch(() => undefined);
      await expect(rig.front.run.start(undefined, { file: 'sim.yaml' })).rejects.toMatchObject({
        code: 'busy',
        outcome: 'refused',
      });
      expect(rig.http.requests.filter((r) => r.request.path === 'api/run')).toHaveLength(1);
    });

    it('refuses a start with a field it does not know, without sending', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      const misspelt = { scenaro: 'drive' } as unknown as Parameters<typeof rig.front.run.start>[0];
      await expect(rig.front.run.start(misspelt, { file: 'sim.yaml' })).rejects.toMatchObject({
        code: 'unknown-field',
        outcome: 'refused',
      });
      expect(rig.http.requests.filter((r) => r.request.path === 'api/run')).toHaveLength(0);
    });

    it('refuses an argument of the wrong form without sending', async () => {
      const rig = await running(pausedState(1));
      await expect(rig.front.time.step(0)).rejects.toMatchObject({ code: 'invalid-argument' });
      await expect(rig.front.time.step(Number.NaN)).rejects.toMatchObject({
        code: 'invalid-argument',
      });
      await expect(rig.front.time.setSpeed(-1)).rejects.toMatchObject({ code: 'invalid-argument' });
      await expect(rig.front.time.runUntil(-5)).rejects.toMatchObject({ code: 'invalid-argument' });
      rig.http.expectNone('api/time');
    });

    it('fails what is waiting as unknown when the front is closed', async () => {
      const rig = await running();
      const pause = rig.front.time.pause().catch((e: unknown) => e as ActionError);
      rig.timeBody();
      rig.front.close();
      expect(await pause).toMatchObject({ code: 'closed', outcome: 'unknown' });
    });
  });

  describe('the run operations', () => {
    it('sends a stop whatever the connection says, and a second stop is the same request', async () => {
      const rig = await running();
      rig.socket.fail();
      await rig.turn();
      expect(rig.front.status.get().link).toBe('offline');
      const first = rig.front.run.stop();
      const second = rig.front.run.stop();
      rig.http.expectOne('api/stop-session').flush({ instance: 'x', run_id: '1' });
      await Promise.all([first, second]);
      rig.http.expectNone('api/stop-session');
    });

    it('keeps the stop target when clearing a problem synchronously changes the session', async () => {
      const rig = await running();
      const invalid = { scenaro: 'x' } as unknown as Parameters<typeof rig.front.run.start>[0];
      await expect(rig.front.run.start(invalid, { file: 'sim.yaml' })).rejects.toThrow();
      let switched = false;
      const unsubscribe = rig.front.status.subscribe((status) => {
        if (status.problem || switched) return;
        switched = true;
        void rig.deliver({ type: 'session', session: summary({}, 2) });
      });
      const stop = rig.front.run.stop();
      const request = rig.http.expectOne('api/stop-session');
      expect(request.request.body).toMatchObject({ run_id: '1', instance: 'x' });
      request.flush({ instance: 'x', run_id: '1' });
      await stop;
      unsubscribe();
    });

    it('keeps stops of consecutive starts as separate operations', async () => {
      const rig = await running();
      const start1 = rig.front.run.start({}, { file: 'sim.yaml' });
      const stop1 = rig.front.run.stop();
      rig.http.expectOne('api/run').flush({ instance: 'x', run_id: '2' });
      await start1;
      await rig.turn();
      const request1 = rig.http.expectOne('api/stop-session');
      const start2 = rig.front.run.start({}, { file: 'sim.yaml' });
      const stop2 = rig.front.run.stop();
      rig.http.expectOne('api/run').flush({ instance: 'x', run_id: '3' });
      await start2;
      await rig.turn();
      const request2 = rig.http.expectOne('api/stop-session');
      expect(request1.request.body).toMatchObject({ run_id: '2', instance: 'x' });
      expect(request2.request.body).toMatchObject({ run_id: '3', instance: 'x' });
      request1.flush({ instance: 'x', run_id: '2' });
      request2.flush({ instance: 'x', run_id: '3' });
      await Promise.all([stop1, stop2]);
    });

    it('does not offer a force stop for the old run while the next run starts', async () => {
      const rig = await running();
      await rig.deliver({ type: 'session', session: summary({ stop: 'requested' }) });
      const start = rig.front.run.start({}, { file: 'sim.yaml' });
      expect(rig.front.controls.get().force_stop.ok).toBe(false);
      await expect(rig.front.run.forceStop({ run_id: '1', instance: 'x' })).rejects.toMatchObject({
        code: 'starting',
      });
      rig.http.expectNone('api/stop-session');
      rig.http.expectOne('api/run').flush({ instance: 'x', run_id: '2' });
      await start;
      await expect(rig.front.run.forceStop({ run_id: '1', instance: 'x' })).rejects.toMatchObject({
        code: 'session-changed',
      });
      rig.http.expectNone('api/stop-session');
    });

    it('turns force_stop on after a stop has been asked for', async () => {
      const rig = await running();
      const stop = rig.front.run.stop();
      expect(rig.front.controls.get().stop.reason?.code).toBe('busy');
      rig.http.expectOne('api/stop-session').flush({ instance: 'x', run_id: '1' });
      await stop;
      expect(rig.front.status.get().run).toBe('stopping');
      expect(rig.front.controls.get().force_stop.ok).toBe(true);
      expect(rig.front.controls.get().stop.reason?.code).toBe('already-stopping');
    });

    it('does not treat the previous run stop as a stop of the new run', async () => {
      const rig = await running();
      const firstStop = rig.front.run.stop();
      const firstRequest = rig.http.expectOne('api/stop-session');
      await rig.deliver({ type: 'session', session: summary({}, 2) });
      expect(rig.front.controls.get().force_stop.reason?.code).toBe('not-stopping');
      expect(rig.front.controls.get().stop.ok).toBe(true);
      const secondStop = rig.front.run.stop();
      const secondRequest = rig.http.expectOne('api/stop-session');
      expect(secondRequest.request.body).toMatchObject({ run_id: '2', instance: 'x' });
      firstRequest.flush({ instance: 'x', run_id: '1' });
      await firstStop;
      expect(rig.front.controls.get().stop.reason?.code).toBe('busy');
      secondRequest.flush({ instance: 'x', run_id: '2' });
      await secondStop;
    });

    it('starts the run with what it was given, and says starting meanwhile', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      const start = rig.front.run.start(
        { speed: 'max', start_paused: true, disable: ['Logger'] },
        { file: 'sim.yaml' },
      );
      expect(rig.front.status.get().run).toBe('starting');
      expect(rig.front.controls.get().start.reason?.code).toBe('busy');
      const request = rig.http.expectOne('api/run');
      expect(request.request.body).toMatchObject({
        file: 'sim.yaml',
        speed: 'max',
        start_paused: true,
        disable: ['Logger'],
      });
      request.flush({ instance: 'x', run_id: '1' });
      await start;
      expect(rig.front.status.get().run).not.toBe('none');
    });

    it('has left the ended state of the run before it when start resolves (the answer is applied first)', async () => {
      const rig = await open();
      const ended = summary({
        state: 'ended',
        launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
        allowed: serverAllowed(2, startOk, notRunning),
      });
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: ended,
          allowed: serverAllowed(2, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: '1', last_seq: 0 },
      );
      // A run that is over (without the launcher's end event it reads as crashed): both are what a wait for the end tests
      expect(['ended', 'crashed']).toContain(rig.front.status.get().run);
      const start = rig.front.run.start({}, { file: 'sim.yaml' });
      rig.http.expectOne('api/run').flush({ instance: 'x', run_id: '2' });
      await start;
      // `until(status, s => s.run === 'ended')` after `await start()` waits for the new run, not for the old one
      expect(['ended', 'crashed']).not.toContain(rig.front.status.get().run);
      expect(rig.front.status.get().result).toBeNull();
    });

    it('sends the intent as the request file has it: every key it was given, and no null', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );
      const start = rig.front.run.start(
        {
          variant: 'small',
          params: { gain: 0.5, on: true },
          scenario: 'scenarios/s.yaml',
          disable: ['Logger'],
          enable: ['Spare'],
          manual: ['Dashboard'],
          strict: true,
          connect: { ETH_EXT: 'eth0' },
          time_policy: 'free',
        },
        { file: 'sim.yaml' },
      );
      const request = rig.http.expectOne('api/run');
      expect(request.request.body).toEqual({
        file: 'sim.yaml',
        variant: 'small',
        params: { gain: 0.5, on: true },
        scenario: 'scenarios/s.yaml',
        disable: ['Logger'],
        enable: ['Spare'],
        manual: ['Dashboard'],
        strict: true,
        connect: { ETH_EXT: 'eth0' },
        time_policy: 'free',
      });
      request.flush({ instance: 'x', run_id: '1' });
      await start;
    });

    it('rejects a start that got no answer as unknown', async () => {
      const rig = await open();
      const start = rig.front.run
        .start(undefined, { file: 'sim.yaml' })
        .catch((e: unknown) => e as ActionError);
      rig.http.expectOne('api/run').fail(HttpError.noAnswer('POST', 'api/run', 240_000));
      expect(await start).toMatchObject({ code: 'no-answer', outcome: 'unknown' });
    });
  });

  describe('listing the scenarios', () => {
    const answer = (file: string) => ({
      file,
      directory: 'scenarios',
      found: true,
      scenarios: [{ path: 'scenarios/a.yaml', name: 'a.yaml' }],
      truncated: false,
    });

    it('lists the scenarios of the file that is given, as the entries of the list', async () => {
      const rig = await open();
      const list = rig.front.scenarios({ file: 'sim.yaml' });
      const request = rig.http.expectOne('api/scenarios?file=sim.yaml');
      expect(request.request.method).toBe('GET');
      request.flush(answer('sim.yaml'));
      expect(await list).toEqual([{ path: 'scenarios/a.yaml', name: 'a.yaml' }]);
    });

    it('asks for the start-up file when none is given (as run.start does), and rejects without one', async () => {
      const rig = await open({ ...(app as object), initial_file: 'start.yaml' });
      const list = rig.front.scenarios();
      rig.http.expectOne('api/scenarios?file=start.yaml').flush(answer('start.yaml'));
      expect(await list).toHaveLength(1);
      // The app example has no start-up file
      const bare = await open();
      await expect(bare.front.scenarios()).rejects.toMatchObject({
        code: 'no-file',
        outcome: 'refused',
      });
      bare.http.expectNone('api/scenarios');
    });

    it("rejects with the server's code as an ActionError", async () => {
      const rig = await open();
      const list = rig.front
        .scenarios({ file: 'gone.yaml' })
        .catch((e: unknown) => e as ActionError);
      rig.http
        .expectOne('api/scenarios?file=gone.yaml')
        .flush(
          { error: { code: 'unknown-file', message: 'no such file.', detail: null } },
          { status: 404, statusText: 'Not Found' },
        );
      expect(await list).toBeInstanceOf(ActionError);
      expect(await list).toMatchObject({ code: 'unknown-file' });
    });
  });

  describe('connecting outside', () => {
    /** The answer of api/connections with `networks` as the launcher's list of that network. */
    const listed = (networks: unknown[], more: Record<string, unknown> = {}) => ({
      file: 'connect.yaml',
      network: 'ETH_EXT',
      exit_code: 0,
      connections: {
        vehicle_sil_connections: 1,
        result: 'listed',
        networks,
        findings: [],
        ...more,
      },
    });

    it('lists one network of the file when asked, as one list of every backend in the answer order', async () => {
      const rig = await open();
      const example = (await httpExamples()).find((e) => e.name === 'connections')!;
      const list = rig.front.connections({ network: 'ETH_EXT' }, { file: 'connect.yaml' });
      const request = rig.http.expectOne('api/connections');
      expect(request.request).toMatchObject({
        method: 'POST',
        body: { file: 'connect.yaml', network: 'ETH_EXT' },
      });
      request.flush(example.response);
      const entries = await list;
      expect(entries[0]).toEqual({
        backend: 'packet',
        value: 'fake0',
        label: 'fake0',
        detail: 'up, connected; Fake Ethernet adapter with a device that answers',
        usable: true,
        reason: null,
      });
      expect(entries.at(-1)).toEqual({
        backend: 'tap',
        value: 'tap:lo',
        label: 'lo',
        detail: 'not Ethernet (link type 772), down',
        usable: false,
        reason: 'not Ethernet (link type 772)',
      });
      expect(entries.map((e) => e.backend)).toEqual([
        ...Array<string>(entries.length - 1).fill('packet'),
        'tap',
      ]);
    });

    it("gives a backend that cannot list as an entry with the launcher's sentence, not as a failure", async () => {
      const rig = await open();
      const list = rig.front.connections({ network: 'ETH_EXT' }, { file: 'connect.yaml' });
      rig.http.expectOne('api/connections').flush(
        listed(
          [
            {
              name: 'ETH_EXT',
              backend: 'packet',
              list: {
                backend: 'packet',
                targets: [
                  { value: 'eth0', label: 'eth0', detail: 'up', usable: true, reason: null },
                ],
                error: null,
              },
            },
            {
              name: 'ETH_EXT',
              backend: 'tap',
              list: {
                backend: null,
                targets: [],
                error: { code: 'tool-missing', message: 'no bridge.' },
              },
            },
            { name: 'ETH_EXT', backend: null, list: null },
          ],
          { result: 'incomplete' },
        ),
      );
      expect(await list).toEqual([
        {
          backend: 'packet',
          value: 'eth0',
          label: 'eth0',
          detail: 'up',
          usable: true,
          reason: null,
        },
        { backend: 'tap', error: 'no bridge.' },
      ]);
    });

    it('gives an error finding of a file that cannot be listed as an entry of its own', async () => {
      const rig = await open();
      const list = rig.front.connections({ network: 'ETH_EXT' }, { file: 'connect.yaml' });
      const answer = listed([], {
        findings: [
          { severity: 'error', code: 'yaml', message: 'connect.yaml: line 3 is not YAML.' },
          { severity: 'warning', code: 'w', message: 'only a warning.' },
        ],
      });
      rig.http.expectOne('api/connections').flush({ ...answer, exit_code: 1 });
      expect(await list).toEqual([{ backend: null, error: 'connect.yaml: line 3 is not YAML.' }]);
    });

    it("rejects with the server's code, and refuses without a file", async () => {
      const rig = await open();
      const list = rig.front
        .connections({ network: 'ETH_EXT' }, { file: 'connect.yaml' })
        .catch((e: unknown) => e as ActionError);
      rig.http
        .expectOne('api/connections')
        .flush(
          { error: { code: 'check-busy', message: 'a check is running.', detail: null } },
          { status: 503, statusText: 'Service Unavailable' },
        );
      expect(await list).toMatchObject({ code: 'check-busy', outcome: 'refused' });
      // The app example has no start-up file
      await expect(rig.front.connections({ network: 'ETH_EXT' })).rejects.toMatchObject({
        code: 'no-file',
      });
      rig.http.expectNone('api/connections');
      expect(rig.front.status.get().problem).toBeNull();
    });

    it('checks the intent as a start would send it, and leaves a refusal out of the problem', async () => {
      const rig = await open();
      const example = (await httpExamples()).find((e) => e.name === 'check-connect')!;
      const check = rig.front.check(
        { connect: { ETH_EXT: 'fake0' }, time_policy: 'free' },
        { file: 'connect.yaml' },
      );
      const request = rig.http.expectOne('api/check');
      expect(request.request.body).toEqual({
        file: 'connect.yaml',
        disable: [],
        connect: { ETH_EXT: 'fake0' },
        time_policy: 'free',
      });
      request.flush(example.response);
      const result = await check;
      expect(result.findings.map((f) => f.code)).toContain('outside-speed-set');
      expect(result.raw?.['time_lock']).toEqual({ locked: true, because: ['ETH_EXT'] });

      const refused = rig.front
        .check({ connect: { ETH_EXT: 'fake9' } }, { file: 'connect.yaml' })
        .catch((e: unknown) => e as ActionError);
      rig.http
        .expectOne('api/check')
        .flush(
          { error: { code: 'target-not-listed', message: 'list it again.', detail: null } },
          { status: 400, statusText: 'Bad Request' },
        );
      expect(await refused).toMatchObject({ code: 'target-not-listed', message: 'list it again.' });
      expect(rig.front.status.get().problem).toBeNull();
    });

    it('sends one check at a time, shares a check of the same intent, and drops a waiting check that every caller aborted', async () => {
      const rig = await open();
      const file = { file: 'connect.yaml' };
      const passed = (await httpExamples()).find((e) => e.name === 'check-connect')!.response;
      const first = rig.front.check({ disable: ['A'], manual: ['M'] }, file);
      // The same intent and file, also with the fields in another order: the check on its way
      const same = rig.front.check({ manual: ['M'], disable: ['A'] }, file);
      expect(same).toBe(first);
      const going = rig.http.expectOne('api/check');
      // Another intent waits for the answer to the one on its way
      const aborts = new AbortController();
      const dropped = rig.front
        .check({ disable: ['B'] }, { ...file, signal: aborts.signal })
        .catch((e: unknown) => e);
      const kept = new AbortController();
      const next = rig.front.check({ disable: ['C'] }, { ...file, signal: kept.signal });
      const shared = rig.front.check({ disable: ['C'] }, file);
      expect(shared).toBe(next);
      kept.abort(); // another caller still wants it
      rig.http.expectNone('api/check');
      aborts.abort(new Error('changed'));
      going.flush(passed);
      await first;
      expect(await dropped).toMatchObject({ message: 'changed' });
      // The aborted check is not sent: the one after it is
      const sent = await vi.waitFor(() => rig.http.expectOne('api/check'));
      expect(sent.request.body).toEqual({ file: 'connect.yaml', disable: ['C'] });
      sent.flush(passed);
      await next;
      // An answered check is not shared: the same intent again is a new check
      void rig.front.check({ disable: ['A'] }, file);
      rig.http.expectOne('api/check').flush(passed);
      // A signal that has aborted already sends nothing
      const late = new AbortController();
      late.abort(new Error('late'));
      await expect(rig.front.check({}, { ...file, signal: late.signal })).rejects.toThrow('late');
      rig.http.expectNone('api/check');
    });

    it('sends the next check after a refused one, checks again afresh when asked, and sends no waiting check after close', async () => {
      const rig = await open();
      const file = { file: 'connect.yaml' };
      const passed = (await httpExamples()).find((e) => e.name === 'check-connect')!.response;
      const busy = rig.front.check({ disable: ['A'] }, file).catch((e: unknown) => e);
      const going = rig.http.expectOne('api/check');
      // Asked again by the user: not the answer on its way (it may have read the file before it was fixed)
      const fresh = rig.front.check({ disable: ['A'] }, { ...file, fresh: true });
      // ... but a check that waits is shared
      expect(rig.front.check({ disable: ['A'] }, { ...file, fresh: true })).toBe(fresh);
      rig.http.expectNone('api/check');
      going.flush(
        { error: { code: 'check-busy', message: 'checks are waiting.', detail: null } },
        { status: 503, statusText: 'Service Unavailable' },
      );
      expect(await busy).toMatchObject({ code: 'check-busy' });
      // A refused check does not hold up the next one
      const again = await vi.waitFor(() => rig.http.expectOne('api/check'));
      expect(again.request.body).toEqual({ file: 'connect.yaml', disable: ['A'] });
      again.flush(passed);
      await fresh;
      // Closed while one is on its way: the one that waits is not sent
      void rig.front.check({ disable: ['B'] }, file).catch(() => undefined);
      rig.http.expectOne('api/check');
      const waiting = rig.front.check({ disable: ['C'] }, file).catch((e: unknown) => e);
      rig.front.close();
      expect(await waiting).toMatchObject({ code: 'closed', outcome: 'refused' });
      await expect(rig.front.check({}, file)).rejects.toMatchObject({ code: 'closed' });
      rig.http.expectNone('api/check');
    });

    it('refuses to check an intent with a field it does not know, without sending', async () => {
      const rig = await open();
      const misspelt = { conect: { ETH_EXT: 'fake0' } } as unknown as Parameters<
        typeof rig.front.check
      >[0];
      await expect(rig.front.check(misspelt, { file: 'connect.yaml' })).rejects.toMatchObject({
        code: 'unknown-field',
        outcome: 'refused',
      });
      rig.http.expectNone('api/check');
      expect(rig.front.status.get().problem).toBeNull();
    });

    describe('the text of a configuration file', () => {
      const example = async (name: string) =>
        (await httpExamples()).find((e) => e.name === name)!.response;

      it('reads the saved text, the start-up file by default, and leaves a refusal out of the problem', async () => {
        const rig = await open();
        const config = await example('config');
        const read = rig.front.readConfig({ file: 'connect.yaml' });
        const sent = rig.http.expectOne('api/config?file=connect.yaml');
        sent.flush(config);
        expect(await read).toMatchObject({ text: expect.any(String), stamp: expect.any(String) });
        // The app example has no start-up file
        await expect(rig.front.readConfig()).rejects.toMatchObject({ code: 'no-file' });
        rig.http.expectNone('api/config?file=connect.yaml');
        const refused = rig.front
          .readConfig({ file: 'x.yaml' })
          .catch((e: unknown) => e as ActionError);
        rig.http
          .expectOne('api/config?file=x.yaml')
          .flush(
            { error: { code: 'config-too-large', message: 'too large.', detail: null } },
            { status: 413, statusText: 'Payload Too Large' },
          );
        expect(await refused).toMatchObject({ code: 'config-too-large', outcome: 'refused' });
        expect(rig.front.status.get().problem).toBeNull();
      });

      it('saves a text with its stamp, the start-up file by default, and leaves a refusal out of the problem', async () => {
        const rig = await open();
        const saving = rig.front.saveConfig({
          file: 'simulation.yaml',
          text: 'vehicle_sil_simulation: 2\r\n',
          stamp: 'a'.repeat(64),
        });
        const sent = rig.http.expectOne('api/config');
        expect(sent.request.method).toBe('POST');
        expect(sent.request.body).toEqual({
          file: 'simulation.yaml',
          text: 'vehicle_sil_simulation: 2\r\n',
          stamp: 'a'.repeat(64),
        });
        const saved = await example('save-config');
        sent.flush(saved);
        expect(await saving).toEqual(saved);
        // A new file: stamp null is sent as it is
        const making = rig.front.saveConfig({ file: 'new.yaml', text: '', stamp: null });
        const create = rig.http.expectOne('api/config');
        expect(create.request.body).toEqual({ file: 'new.yaml', text: '', stamp: null });
        create.flush({ file: 'new.yaml', stamp: 'b'.repeat(64) });
        expect(await making).toEqual({ file: 'new.yaml', stamp: 'b'.repeat(64) });
        // The app example has no start-up file
        await expect(rig.front.saveConfig({ text: '', stamp: null })).rejects.toMatchObject({
          code: 'no-file',
        });
        rig.http.expectNone('api/config');
        // The server's refusal, with the launcher's findings
        const refusal = (await httpExamples()).find((e) => e.name === 'error-commands-changed')!;
        const refused = rig.front
          .saveConfig({ file: 'simulation.yaml', text: 'x', stamp: 'a'.repeat(64) })
          .then(
            () => null,
            (e: unknown) => e as ActionError,
          );
        rig.http
          .expectOne('api/config')
          .flush(refusal.response, { status: refusal.status, statusText: 'Forbidden' });
        const error = await refused;
        expect(error).toMatchObject({ code: 'commands-changed', outcome: 'refused' });
        expect(error?.findings.map((f) => f.code)).toEqual(['commands-changed']);
        expect(rig.front.status.get().problem).toBeNull();
      });

      it("carries the server's allowed.save, with its caution, in controls.save", async () => {
        const rig = await open();
        const caution = {
          code: 'command-changes-allowed',
          message: 'the GUI server was started with --allow-command-changes',
        };
        const hello = (save: object) => ({
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: { ...serverAllowed(1, startOk, notRunning), save },
          replay: null,
        });
        await rig.deliver(hello({ ...startOk, caution }), {
          type: 'synced',
          run_id: null,
          last_seq: null,
        });
        expect(rig.front.controls.get().save).toEqual({ ok: true, reason: null, caution });
        const disabled = {
          ok: false,
          reason: 'save-disabled',
          message: 'the GUI server was started with --no-save',
          caution: null,
        };
        await rig.deliver({
          type: 'allowed',
          run_id: null,
          allowed: { ...serverAllowed(2, startOk, notRunning), save: disabled },
        });
        expect(rig.front.controls.get().save).toEqual({
          ok: false,
          reason: { code: 'save-disabled', message: disabled.message },
          caution: null,
        });
      });

      it('checks a text with the text in the body, and refuses a field that a text does not take', async () => {
        const rig = await open();
        const check = rig.front.check(
          { variant: 'v', strict: true, vehicle_sil_run_request: 1 },
          { file: 'connect.yaml', text: 'vehicle_sil_simulation: 2\n' },
        );
        const request = rig.http.expectOne('api/check');
        expect(request.request.body).toEqual({
          file: 'connect.yaml',
          text: 'vehicle_sil_simulation: 2\n',
          vehicle_sil_run_request: 1,
          variant: 'v',
          strict: true,
        });
        request.flush(await example('check-text'));
        expect((await check).findings).toBeDefined();
        // The types refuse this; plain JavaScript can still send it
        const wrong = { disable: ['A'] } as unknown as { variant?: string };
        await expect(
          rig.front.check(wrong, { file: 'connect.yaml', text: '' }),
        ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });
        rig.http.expectNone('api/check');
      });

      it('does not share a check of a text with the check of the page, in either order', async () => {
        const rig = await open();
        const file = { file: 'connect.yaml' };
        const passed = await example('check-connect');
        const page = rig.front.check({}, file);
        const going = rig.http.expectOne('api/check');
        const text = rig.front.check({}, { ...file, text: 'a: 1\n' });
        const again = rig.front.check({}, { ...file, text: 'a: 1\n' });
        expect(text).not.toBe(page);
        expect(again).not.toBe(text);
        // The page's check asked while the texts wait is its own, not theirs
        const pageAfter = rig.front.check({}, { ...file, fresh: true });
        expect(pageAfter).not.toBe(text);
        expect(rig.front.check({}, { ...file, fresh: true })).toBe(pageAfter);
        rig.http.expectNone('api/check');
        going.flush(passed);
        await page;
        const second = await vi.waitFor(() => rig.http.expectOne('api/check'));
        expect(second.request.body).toMatchObject({ text: 'a: 1\n' });
        second.flush(await example('check-text'));
        await text;
        const third = await vi.waitFor(() => rig.http.expectOne('api/check'));
        expect(third.request.body).toMatchObject({ text: 'a: 1\n' });
        third.flush(await example('check-text'));
        await again;
        const last = await vi.waitFor(() => rig.http.expectOne('api/check'));
        expect(last.request.body).toEqual({ file: 'connect.yaml', disable: [] });
        last.flush(passed);
        await pageAfter;
      });

      it("keeps the failure of a check of a text out of the problem, with the server's code", async () => {
        const rig = await open();
        const failed = rig.front
          .check({}, { file: 'connect.yaml', text: 'x' })
          .catch((e: unknown) => e as ActionError);
        rig.http
          .expectOne('api/check')
          .flush(
            { error: { code: 'text-too-large', message: 'shorten it.', detail: null } },
            { status: 413, statusText: 'Payload Too Large' },
          );
        expect(await failed).toMatchObject({ code: 'text-too-large', message: 'shorten it.' });
        expect(rig.front.status.get().problem).toBeNull();
      });

      it('describes again the saved file in the line, shares only a describe of the same file and variant, and keeps its failure out of the problem', async () => {
        const rig = await open();
        const file = { file: 'connect.yaml' };
        const page = rig.front.check({}, file);
        const going = rig.http.expectOne('api/check');
        // The line is not empty: these two wait behind the page check
        const described = rig.front.describeAgain({ ...file, variant: 'v' });
        const again = rig.front.describeAgain({ ...file, variant: 'v' });
        const other = rig.front.describeAgain({ ...file, variant: 'w' });
        const plain = rig.front.describeAgain(file);
        expect(again).toBe(described);
        expect(other).not.toBe(described);
        expect(plain).not.toBe(described);
        rig.http.expectNone('api/describe');
        // A page check is not a describe, and neither is a check of a text
        expect(rig.front.check({}, { ...file, fresh: true })).not.toBe(described);
        going.flush(await example('check-connect'));
        await page;
        const sent = await vi.waitFor(() => rig.http.expectOne('api/describe'));
        expect(sent.request.body).toEqual({ file: 'connect.yaml', variant: 'v' });
        sent.flush(await example('describe'));
        expect((await described).findings).toBeDefined();
        const next = await vi.waitFor(() => rig.http.expectOne('api/describe'));
        expect(next.request.body).toEqual({ file: 'connect.yaml', variant: 'w' });
        // The server refuses: the failure is the caller's, with the server's code
        const failed = other.catch((e: unknown) => e as ActionError);
        next.flush(
          { error: { code: 'check-busy', message: 'checks are waiting.', detail: null } },
          { status: 503, statusText: 'Service Unavailable' },
        );
        expect(await failed).toMatchObject({ code: 'check-busy', outcome: 'refused' });
        expect(rig.front.status.get().problem).toBeNull();
        await expect(rig.front.describeAgain()).rejects.toMatchObject({ code: 'no-file' });
        // Closed while one waits: it is not sent
        const waiting = plain.catch((e: unknown) => e as ActionError);
        rig.front.close();
        expect(await waiting).toMatchObject({ code: 'closed' });
      });
    });

    const row = (more: Record<string, unknown> = {}) => ({
      network: 'ETH_EXT',
      target: 'fake0',
      participant: 'bridge.ETH_EXT',
      link: 'up',
      bus: null,
      nothing_received: false,
      condition: 'ok',
      reasons: [],
      ...more,
    });
    const counts = (network: string, to: number, from: number) => ({
      event: 'outside',
      t_ms: 1,
      network,
      to_outside: to,
      from_outside: from,
    });

    it('shows the rows of the state with the counts of the outside events, while the run goes', async () => {
      const rig = await running(stateEvent(1, { outside: [row()] }));
      expect(rig.front.outside.get()).toEqual([
        {
          network: 'ETH_EXT',
          target: 'fake0',
          participant: 'bridge.ETH_EXT',
          link: 'up',
          bus: null,
          condition: 'ok',
          reasons: [],
          nothing_received: false,
          to_outside: null,
          from_outside: null,
        },
      ]);
      await rig.deliver(event(counts('ETH_EXT', 5, 2)), event(counts('ETH_EXT', 9, 4)));
      expect(rig.front.outside.get()[0]).toMatchObject({ to_outside: 9, from_outside: 4 });
      await rig.deliver(
        event(
          stateEvent(2, {
            outside: [row({ link: 'down', condition: 'interrupted', reasons: ['link-down'] })],
          }),
        ),
      );
      expect(rig.front.outside.get()[0]).toMatchObject({
        condition: 'interrupted',
        reasons: ['link-down'],
        to_outside: 9,
      });
      // The launcher has ended: no row
      await rig.deliver({
        type: 'session',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
        }),
      });
      expect(rig.front.outside.get()).toEqual([]);
    });

    it('starts again with no counts in the next run', async () => {
      const rig = await running(stateEvent(1, { outside: [row()] }));
      await rig.deliver(event(counts('ETH_EXT', 5, 2)));
      await rig.deliver({ type: 'session', session: summary({}, 2) });
      expect(rig.front.outside.get()).toEqual([]);
      await rig.deliver(event(stateEvent(3, { outside: [row()] }), 2));
      expect(rig.front.outside.get()[0]).toMatchObject({ to_outside: null, from_outside: null });
    });

    it('reads the rows from the summary of a page that joins a run on its way', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: summary({ launcher_state: { ...stateEvent(4, { outside: [row()] }), seq: 14 } }),
          allowed: serverAllowed(3, alreadyRunning),
          replay: null,
        },
        { type: 'synced', run_id: '1', last_seq: 40 },
      );
      expect(rig.front.outside.get().map((o) => o.network)).toEqual(['ETH_EXT']);
    });
  });

  /** A run with the variable Engine.engine.offset, and the launcher's state `state`. */
  async function withVariable(state: unknown = stateEvent(1)): Promise<Rig> {
    const rig = await open();
    await rig.deliver(
      {
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'x',
        session: {
          id: '1',
          state: 'running',
          launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
          stop: 'none',
          variables: {
            state: 'ready',
            reason: null,
            peers: [
              {
                name: 'Engine',
                connected: true,
                variables: [
                  {
                    name: 'engine.offset',
                    type: 'real',
                    role: 'input',
                    writable: true,
                    unit: 'rpm',
                    min: null,
                    max: null,
                    step: null,
                    description: '',
                    enum: null,
                  },
                ],
              },
            ],
            peers_omitted: 0,
            variables_omitted: 0,
            operations: [],
          },
        },
        replay: null,
      },
      event(state),
      { type: 'synced', run_id: '1', last_seq: null },
    );
    return rig;
  }

  describe('a variable', () => {
    const named = 'Engine.engine.offset';

    it('writes without a subscription, and resolves when the write is applied', async () => {
      const rig = await withVariable();
      const variable = rig.front.variable(named);
      let writing = false;
      const done = variable.write(50).then(() => (writing = true));
      const request = rig.http.expectOne('api/write');
      expect(request.request.body).toMatchObject({ writes: { Engine: { 'engine.offset': 50 } } });
      expect(variable.get().writing).toBe(true);
      request.flush({ run_id: '1', operation: 4 });
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 4,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 200, late: false, reason: null } },
      });
      await done;
      expect(writing).toBe(true);
      expect(variable.get()).toMatchObject({
        writing: false,
        problem: null,
        unit: 'rpm',
        can_write: true,
        why: null,
      });
    });

    it('shows the availability beside the other conditions and leaves them alone', async () => {
      const rig = await withVariable();
      const variable = rig.front.variable(named);
      const before = variable.get();
      expect(before.availability).toBe('active');
      await rig.deliver({
        type: 'availability',
        run_id: '1',
        peers: { Engine: { 'engine.offset': 'inactive' } },
      });
      const after = variable.get();
      expect(after.availability).toBe('inactive');
      expect({ ...after, availability: 'active' }).toEqual(before);
    });

    it("says why a variable cannot be written in the launcher's words, and follows the launcher when it changes", async () => {
      const playing =
        'this run plays a scenario, and until it ends only the scenario writes variables';
      const rig = await withVariable(
        stateEvent(1, { scenario: true }, { write: refusal('scenario-running', playing) }),
      );
      const variable = rig.front.variable(named);
      expect(variable.get()).toMatchObject({
        can_write: false,
        why: { code: 'scenario-running', message: playing },
        block: 'launcher',
      });
      // A launcher that takes the write during a scenario: nothing in the client says otherwise
      await rig.deliver(event(stateEvent(2, { scenario: true })));
      expect(variable.get()).toMatchObject({ can_write: true, why: null, block: null });
    });

    it("shows a refused write in the view and rejects with the server's code", async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: summary(),
          allowed: serverAllowed(3, alreadyRunning),
          replay: null,
        },
        { type: 'synced', run_id: '1', last_seq: null },
      );
      const variable = rig.front.variable({ peer: 'Engine', name: 'engine.offset' });
      const write = variable.write(1).catch((e: unknown) => e as ActionError);
      await rig.turn();
      const requests = rig.http.requests.filter((r) => r.request.path === 'api/write');
      if (requests.length > 0) {
        requests[0].flush(
          {
            error: {
              code: 'not-writable',
              message: 'engine.offset cannot be written',
              detail: null,
            },
          },
          { status: 409, statusText: 'Conflict' },
        );
      }
      const error = await write;
      expect(error).toBeInstanceOf(ActionError);
      expect((error as ActionError).outcome).toBe('refused');
      expect(variable.get().problem?.code).toBe((error as ActionError).code);
    });

    it('cuts the name at the first dot, and refuses a name without one', () => {
      return open().then((rig) => {
        expect(() => rig.front.variable('Engine')).toThrow(/ECU.variable/);
        expect(rig.front.variable(named)).toBe(
          rig.front.variable({ peer: 'Engine', name: 'engine.offset' }),
        );
      });
    });

    it('gives the same view object while nothing changed, with and without a subscription', async () => {
      const rig = await open();
      const variable = rig.front.variable(named);
      expect(variable.get()).toBe(variable.get());
      const stop = variable.subscribe(() => undefined);
      await rig.turn();
      expect(variable.get()).toBe(variable.get());
      stop();
      stop();
      expect(variable.get().value).toBeNull();
    });

    it('forwards optional keepalive on variable.write() without changing the request body', async () => {
      const rig = await running();
      await rig.deliver({
        type: 'variables',
        run_id: '1',
        variables: {
          state: 'ready',
          reason: null,
          peers: [
            {
              name: 'Engine',
              connected: true,
              variables: [
                {
                  name: 'engine.offset',
                  type: 'real',
                  role: 'input',
                  writable: true,
                  unit: 'rpm',
                  min: null,
                  max: null,
                  step: null,
                  description: '',
                  enum: null,
                },
              ],
            },
          ],
          peers_omitted: 0,
          variables_omitted: 0,
          operations: [],
        },
      });
      const variable = rig.front.variable(named);
      const release = variable.write(0, { keepalive: true });
      const sent = rig.http.expectOne('api/write');
      expect(sent.request.keepalive).toBe(true);
      expect(sent.request.body).toEqual({
        run_id: '1',
        instance: 'x',
        writes: { Engine: { 'engine.offset': 0 } },
      });
      sent.flush({ run_id: '1', operation: 7 });
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 7,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 500, late: false, reason: null } },
      });
      await expect(release).resolves.toEqual({
        session: '1',
        seq: 7,
        peer: 'Engine',
        name: 'engine.offset',
        value: 0,
        appliedAtMs: 500,
        late: false,
      });
    });

    it('supports enqueue() -> applied(), competing scheduled writes keeping writing === true, option validation, and AbortSignal', async () => {
      const rig = await withVariable();
      const variable = rig.front.variable(named);

      // Unknown option keys and invalid timeoutMs are refused immediately with ActionError
      await expect(
        variable.enqueue(10, { onAccepted: () => undefined } as never),
      ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });
      await expect(variable.write(10, { timeoutMs: Infinity })).rejects.toMatchObject({
        code: 'invalid-argument',
        outcome: 'refused',
      });
      rig.http.expectNone('api/write');

      // Competing scheduled writes (atMs: 50 and atMs: 100): writing stays true until BOTH settle!
      const q1Promise = variable.enqueue(25, { atMs: 50 });
      const q2Promise = variable.enqueue(75, { atMs: 100 });
      expect(variable.get().writing).toBe(true);
      expect(rig.http.requests.filter((r) => r.request.path === 'api/write')).toHaveLength(2);
      const [req1, req2] = rig.http.requests.splice(0, 2);
      expect(req1.request.body).toMatchObject({
        at_ms: 50,
        writes: { Engine: { 'engine.offset': 25 } },
      });
      expect(req2.request.body).toMatchObject({
        at_ms: 100,
        writes: { Engine: { 'engine.offset': 75 } },
      });
      req1.flush({ run_id: '1', operation: 10 });
      req2.flush({ run_id: '1', operation: 11 });
      const [q1, q2] = await Promise.all([q1Promise, q2Promise]);
      expect(q1).toMatchObject({
        session: '1',
        seq: 10,
        peer: 'Engine',
        name: 'engine.offset',
        value: 25,
      });
      expect(q2).toMatchObject({
        session: '1',
        seq: 11,
        peer: 'Engine',
        name: 'engine.offset',
        value: 75,
      });

      await expect(q1.applied({ bogus: 1 } as never)).rejects.toMatchObject({
        code: 'unknown-field',
        outcome: 'refused',
      });

      const a1 = q1.applied();
      const a2 = q2.applied();
      expect(variable.get().writing).toBe(true);

      // First write settles at 50 ms: writing MUST remain true because operation 11 is still active!
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 10,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 50, late: false, reason: null } },
      });
      await expect(a1).resolves.toEqual({
        session: '1',
        seq: 10,
        peer: 'Engine',
        name: 'engine.offset',
        value: 25,
        appliedAtMs: 50,
        late: false,
      });
      expect(variable.get().writing).toBe(true);

      // Second write settles at 100 ms: now writing clears to false!
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 11,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 100, late: true, reason: null } },
      });
      await expect(a2).resolves.toEqual({
        session: '1',
        seq: 11,
        peer: 'Engine',
        name: 'engine.offset',
        value: 75,
        appliedAtMs: 100,
        late: true,
      });
      expect(variable.get().writing).toBe(false);

      // enqueue() without calling applied() still clears writing once the operation settles
      const q3Promise = variable.enqueue(90, { atMs: 150 });
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 12 });
      await q3Promise;
      expect(variable.get().writing).toBe(true);
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 12,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 150, late: false, reason: null } },
      });
      expect(variable.get().writing).toBe(false);

      // Immediate terminal failure on enqueue() rejects with specific code & outcome
      const q4Promise = variable.enqueue(99, { atMs: 10 });
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 13,
        state: 'late',
        done: true,
        peers: {
          Engine: { state: 'late', applied_at_ms: null, late: false, reason: 'past 10 ms' },
        },
      });
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 13 });
      await expect(q4Promise).rejects.toMatchObject({ code: 'late', outcome: 'refused' });
      expect(variable.get()).toMatchObject({ writing: false, problem: { code: 'late' } });

      // AbortSignal while link === 'connecting' before sync completes
      const connectingRig = await open();
      const abortSync = new AbortController();
      const pendingSyncWrite = connectingRig.front
        .variable(named)
        .enqueue(15, { signal: abortSync.signal });
      abortSync.abort();
      await expect(pendingSyncWrite).rejects.toMatchObject({ code: 'aborted', outcome: 'refused' });
      connectingRig.http.expectNone('api/write');
    });

    it('validates FrontWriteOptions strictly, preserves writing across partial applied() aborts, clears problem on applied/slider success, and isolates run switches & server restarts', async () => {
      const rig = await withVariable();
      const variable = rig.front.variable(named);

      // 1) Strict runtime validation of atMs, late, keepalive, and signal (refused with 0 HTTP requests)
      for (const badOpts of [
        { atMs: Number.NaN },
        { atMs: Number.POSITIVE_INFINITY },
        { atMs: -1 },
        { late: 'bad' as unknown as 'reject' },
        { keepalive: 'yes' as unknown as boolean },
        { signal: {} as unknown as AbortSignal },
      ]) {
        await expect(variable.enqueue(99, badOpts)).rejects.toMatchObject({
          code: 'invalid-argument',
          outcome: 'refused',
        });
      }
      rig.http.expectNone('api/write');

      // 2) Double applied() with independent AbortSignals: aborting 1st keeps writing=true until ON_SETTLED,
      //    and settling as 'applied' clears problem back to null!
      const qDoubleP = variable.enqueue(42, { atMs: 100 });
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 20 });
      const qDouble = await qDoubleP;
      const ac1 = new AbortController();
      const ac2 = new AbortController();
      const p1 = qDouble.applied({ signal: ac1.signal });
      const p2 = qDouble.applied({ signal: ac2.signal });
      ac1.abort();
      await expect(p1).rejects.toMatchObject({ code: 'aborted', outcome: 'unknown' });
      expect(variable.get().writing).toBe(true);
      expect(variable.get().problem?.code).toBe('aborted');

      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 20,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 100, late: false, reason: null } },
      });
      await expect(p2).resolves.toMatchObject({ seq: 20, appliedAtMs: 100 });
      expect(variable.get()).toMatchObject({ writing: false, problem: null });

      // 3) Competing enqueued writes + VariableBinder slider write clear stale problem on subsequent applied
      const wFailP = variable.enqueue(10, { atMs: 50 });
      const wPassP = variable.enqueue(20, { atMs: 150 });
      rig.http.requests[0].flush({ run_id: '1', operation: 21 });
      rig.http.requests[1].flush({ run_id: '1', operation: 22 });
      rig.http.requests.splice(0, 2);
      const [wFail, wPass] = await Promise.all([wFailP, wPassP]);
      const failWait = wFail.applied().catch((e: unknown) => e);
      const passWait = wPass.applied();
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 21,
        state: 'late',
        done: true,
        peers: { Engine: { state: 'late', applied_at_ms: null, late: false, reason: 'missed' } },
      });
      expect(await failWait).toMatchObject({ code: 'late', outcome: 'refused' });
      expect(variable.get()).toMatchObject({ writing: true, problem: { code: 'late' } });

      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 22,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 150, late: false, reason: null } },
      });
      await expect(passWait).resolves.toMatchObject({ seq: 22, appliedAtMs: 150 });
      expect(variable.get()).toMatchObject({ writing: false, problem: null });

      // Slider write via VariableBinder after a FrontVariable failure also clears stale problem
      const wFail2P = variable.enqueue(11, { atMs: 10 });
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 23 });
      const wFail2 = await wFail2P;
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 23,
        state: 'rejected',
        done: true,
        peers: { Engine: { state: 'rejected', applied_at_ms: null, late: false, reason: 'no' } },
      });
      await expect(wFail2.applied()).rejects.toMatchObject({
        code: 'rejected',
        outcome: 'refused',
      });
      expect(variable.get().problem?.code).toBe('rejected');

      const binder = rig.front.low.variables.bind({ peer: 'Engine', name: 'engine.offset' });
      binder.write(33);
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 24 });
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 24,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 200, late: false, reason: null } },
      });
      expect(variable.get().problem).toBeNull();
      binder.close();

      // 4) Run 1 -> Run 2 switch: in-flight Run 1 write rejection and Run 1 problem never leak into Run 2!
      const wOpenRun1P = variable.enqueue(88, { atMs: 500 });
      rig.http.expectOne('api/write').flush({ run_id: '1', operation: 25 });
      const wOpenRun1 = await wOpenRun1P;
      const run1Applied = wOpenRun1.applied().catch((e: unknown) => e);

      await rig.deliver(
        { type: 'session', session: summary({}, 2) },
        {
          type: 'variables',
          run_id: '2',
          variables: {
            state: 'ready',
            reason: null,
            peers: [
              {
                name: 'Engine',
                connected: true,
                variables: [
                  {
                    name: 'engine.offset',
                    type: 'real',
                    role: 'input',
                    writable: true,
                    unit: 'rpm',
                    min: null,
                    max: null,
                    step: null,
                    description: '',
                    enum: null,
                  },
                ],
              },
            ],
            peers_omitted: 0,
            variables_omitted: 0,
            operations: [],
          },
        },
        event(stateEvent(1), '2'),
      );
      expect(await run1Applied).toMatchObject({ code: 'no-answer', outcome: 'unknown' });
      expect(variable.get()).toMatchObject({ writing: false, problem: null });

      // 5) Server restart during WS reconnect backoff includes stale instance in POST api/write -> 409 session-changed
      rig.socket.fail();
      await rig.turn();
      const staleWrite = variable.write(77);
      const staleReq = rig.http.expectOne('api/write');
      expect(staleReq.request.body).toEqual({
        run_id: '2',
        instance: 'x',
        writes: { Engine: { 'engine.offset': 77 } },
      });
      staleReq.flush(
        {
          error: {
            code: 'session-changed',
            message: 'the server instance has changed',
            detail: null,
          },
        },
        { status: 409, statusText: 'Conflict' },
      );
      await expect(staleWrite).rejects.toMatchObject({
        code: 'session-changed',
        outcome: 'refused',
      });
    });
  });

  describe('watch and until', () => {
    it('calls now and on each change, for one source or several, and stops', async () => {
      const rig = await running();
      const seen: string[] = [];
      const stop = watch([rig.front.status, rig.front.clock], (status, clock) =>
        seen.push(`${status.run}@${clock.sim_ms}`),
      );
      expect(seen).toEqual(['running@500']);
      await rig.deliver(event(timeEvent(31, 600)));
      expect(seen.at(-1)).toBe('running@600');
      stop();
      const count = seen.length;
      await rig.deliver(event(timeEvent(32, 700)));
      expect(seen).toHaveLength(count);
    });

    it('resolves with the first value that holds, and rejects on a time-out and on an abort', async () => {
      const rig = await running();
      expect(await until(rig.front.status, (s) => s.run === 'running')).toMatchObject({
        run: 'running',
      });
      const waiting = until(rig.front.status, (s) => s.run === 'ended', { timeoutMs: 500 }).catch(
        (e: unknown) => e as ActionError,
      );
      await vi.advanceTimersByTimeAsync(500);
      const timedOut = await waiting;
      expect(timedOut).toBeInstanceOf(ActionError);
      expect(timedOut).toMatchObject({ code: 'timeout', outcome: 'unknown' });
      expect((timedOut as ActionError).message).toContain('last observed:');
      expect((timedOut as ActionError).message).toContain('"run":"running"');

      // Also verify primitive, updated, nullish, and circular values in timeout messages.
      let listener: ((v: unknown) => void) | undefined;
      let current: unknown = 10;
      const customSource = {
        get: () => current,
        subscribe: (fn: (v: unknown) => void) => {
          listener = fn;
          return () => {
            listener = undefined;
          };
        },
      };
      const numTimeout = until(customSource, () => false, { timeoutMs: 100 }).catch(
        (e: unknown) => e as ActionError,
      );
      current = 42;
      listener?.(42);
      await vi.advanceTimersByTimeAsync(100);
      expect(((await numTimeout) as ActionError).message).toContain('last observed: 42');

      const circular: Record<string, unknown> = { name: 'loop' };
      circular['self'] = circular;
      current = circular;
      const circTimeout = until(customSource, () => false, { timeoutMs: 100 }).catch(
        (e: unknown) => e as ActionError,
      );
      await vi.advanceTimersByTimeAsync(100);
      expect(((await circTimeout) as ActionError).message).toContain('[Circular]');

      current = undefined;
      const undefTimeout = until(customSource, () => false, { timeoutMs: 100 }).catch(
        (e: unknown) => e as ActionError,
      );
      await vi.advanceTimersByTimeAsync(100);
      expect(((await undefTimeout) as ActionError).message).toContain('last observed: undefined');

      // Edge cases: null-prototype / throwing getters / throwing toJSON+toString / throwing Error.message must reject cleanly.
      for (const bad of [
        {
          toJSON() {
            throw new Error('toJSON boom');
          },
          toString() {
            throw new Error('toString boom');
          },
        },
        Object.create(null, {
          x: {
            enumerable: true,
            get() {
              throw new Error('getter boom');
            },
          },
        }),
        {
          toJSON() {
            throw new Error('toJSON boom');
          },
          [Symbol.toPrimitive]() {
            throw new Error('toPrimitive boom');
          },
        },
        Object.defineProperty(new Error('orig'), 'message', {
          get() {
            throw new Error('message getter boom');
          },
        }),
      ]) {
        current = bad;
        const badTimeout = until(customSource, () => false, { timeoutMs: 100 }).catch(
          (e: unknown) => e as ActionError,
        );
        await vi.advanceTimersByTimeAsync(100);
        const err = await badTimeout;
        expect(err).toBeInstanceOf(ActionError);
        expect(err).toMatchObject({ code: 'timeout', outcome: 'unknown' });
        expect((err as ActionError).message).toContain('last observed:');
      }
      const controller = new AbortController();
      const aborted = until(rig.front.status, (s) => s.run === 'ended', {
        signal: controller.signal,
      }).catch((e: unknown) => e as ActionError);
      controller.abort();
      expect(await aborted).toMatchObject({ code: 'aborted' });
    });
  });

  describe('connect', () => {
    it('refuses to connect without options outside a page the server served, and does not remember it', async () => {
      const first = connect();
      await expect(first).rejects.toMatchObject({ code: 'no-token' });
      await expect(connect()).rejects.toMatchObject({ code: 'no-token' });
    });

    it('does not remember a failed connection with options either, and closes what it opened', async () => {
      const http = new FakeHttp();
      const fake = new FakeWebSockets();
      const attempt = connect({
        baseUrl: 'http://127.0.0.1:1/',
        token: 't',
        http: http.transport,
        socket: fake.factory,
      });
      http
        .expectOne('api/app')
        .flush(
          { error: { code: 'unauthorized', message: 'no', detail: null } },
          { status: 401, statusText: 'Unauthorized' },
        );
      await expect(attempt).rejects.toBeDefined();
      expect(fake.sockets.at(-1)?.closedByClient).toBe(true);
    });

    it('connects with options without a token (for --no-auth servers)', async () => {
      const http = new FakeHttp();
      const fake = new FakeWebSockets();
      const app = (await httpExamples()).find((e) => e.name === 'app')?.response;
      const attempt = connect({
        baseUrl: 'http://127.0.0.1:1/',
        http: http.transport,
        socket: fake.factory,
      });
      http.expectOne('api/app').flush(app);
      const front = await attempt;
      expect(fake.sockets).toHaveLength(1);
      fake.last().open();
      expect(fake.last().greeting).toEqual([{ type: 'auth', token: '' }]);
      front.close();
    });
  });

  describe('runtime & prepare', () => {
    it('tracks hello runtime, runtime WS notifications, and front.prepare()', async () => {
      const rig = await open();
      await rig.deliver({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'i1',
        session: null,
        allowed: null,
        replay: null,
        runtime: { mode: 'hot', state: 'idle', config: null, reason: null, message: null },
      });
      expect(rig.front.runtime.get()).toMatchObject({ mode: 'hot', state: 'idle' });

      const prepPromise = rig.front.prepare({ file: 'b.yaml' });
      const prepReq = rig.http.expectOne('api/prepare');
      expect(prepReq.request.body).toEqual({ file: 'b.yaml' });
      prepReq.flush({ instance: 'i1' });
      await rig.deliver({
        type: 'runtime',
        runtime: { mode: 'hot', state: 'ready', config: 'b.yaml', reason: null, message: null },
      });
      const rt = await prepPromise;
      expect(rt).toMatchObject({ mode: 'hot', state: 'idle' });
      expect(rig.front.runtime.get()).toMatchObject({ state: 'ready', config: 'b.yaml' });

      await rig.deliver({
        type: 'runtime',
        runtime: {
          mode: 'cold',
          state: 'cold',
          config: 'fixed.yaml',
          reason: 'fixed-registry',
          message: 'fixed registry_uri',
        },
      });
      expect(rig.front.runtime.get()).toMatchObject({
        mode: 'cold',
        reason: 'fixed-registry',
      });
    });

    it('exposes configs(), recording(), and run.start(..., { onStarted }) on Front', async () => {
      const rig = await open();
      const cfgPromise = rig.front.configs();
      rig.http.expectOne('api/configs').flush({
        files: [
          {
            path: 'simulation.yaml',
            modified: '2026-01-01T00:00:00Z',
            size: 12,
            simulation: true,
          },
        ],
        truncated: false,
      });
      const listed = await cfgPromise;
      expect(listed.files.map((f) => f.path)).toEqual(['simulation.yaml']);

      const recPromise = rig.front.recording();
      rig.http.expectOne('api/recording').flush({ recording: null });
      expect(await recPromise).toBeNull();

      let capturedId: string | null = null;
      const startPromise = rig.front.run.start(
        { stop_at_ms: 100 },
        { file: 'simulation.yaml', onStarted: (s) => (capturedId = s.run_id) },
      );
      const runReq = rig.http.expectOne('api/run');
      runReq.flush({
        instance: 'i1',
        run_id: '42',
      });
      await startPromise;
      expect(capturedId).toBe('42');
    });
  });

  describe('scenario handover', () => {
    it('forwards after_scenario on run.start and check, unlocks writes on handover state, and keeps non-handover false', async () => {
      const rig = await open();
      const scenarioSummary = {
        file: 'scenarios/a.yaml',
        name: 'a',
        step_count: 1,
        steps: [],
        steps_omitted: 0,
        status: null,
        over_at_ms: null,
        timeline_url: null,
      };
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: null, last_seq: null },
      );

      const checkPromise = rig.front.check(
        {
          scenario: 'scenarios/a.yaml',
          after_scenario: 'pause',
          record: ['Dashboard.dashboard.engine_speed'],
          record_after_fail_ms: 300,
        },
        { file: 'sim.yaml' },
      );
      const checkReq = rig.http.expectOne('api/check');
      expect(checkReq.request.body).toEqual({
        file: 'sim.yaml',
        disable: [],
        scenario: 'scenarios/a.yaml',
        after_scenario: 'pause',
        record: ['Dashboard.dashboard.engine_speed'],
        record_after_fail_ms: 300,
      });
      checkReq.flush({
        check: {
          vehicle_sil_check: 1,
          file: 'sim.yaml',
          result: 'passed',
          errors: 0,
          warnings: 0,
          notes: 0,
          stop_at_ms: null,
          participants: [],
          networks: [],
          findings: [],
          scenario: { file: 'scenarios/a.yaml', name: 'a', steps: 1, end_ms: 500 },
        },
      });
      await checkPromise;

      const startPromise = rig.front.run.start(
        {
          scenario: 'scenarios/a.yaml',
          after_scenario: 'pause',
          record: ['Dashboard.dashboard.engine_speed'],
          record_after_fail_ms: 300,
          speed: 'max',
        },
        { file: 'sim.yaml' },
      );
      const runReq = rig.http.expectOne('api/run');
      expect(runReq.request.body).toEqual({
        file: 'sim.yaml',
        disable: [],
        scenario: 'scenarios/a.yaml',
        after_scenario: 'pause',
        record: ['Dashboard.dashboard.engine_speed'],
        record_after_fail_ms: 300,
        speed: 'max',
      });
      runReq.flush({ instance: 'x', run_id: '1' });
      await startPromise;

      // Deliver session running with scenario-running write refusal
      await rig.deliver(
        {
          type: 'session',
          session: summary({
            speed: 'max',
            last_seq: 0,
            last_t_ms: 10,
            sim_ms: 100,
            launcher_state: stateEvent(
              1,
              { seq: 0, scenario: true, handed_over: false, scenario_over_at_ms: null },
              { write: refusal('scenario-running', 'scenario is running') },
            ),
            scenario: scenarioSummary,
          }),
        },
        { type: 'synced', run_id: '1', last_seq: 0 },
        {
          type: 'variables',
          run_id: '1',
          variables: {
            state: 'ready',
            reason: null,
            peers: [
              {
                name: 'Engine',
                connected: true,
                variables: [
                  {
                    name: 'engine.offset',
                    type: 'real',
                    role: 'input',
                    writable: true,
                    unit: 'rpm',
                    min: null,
                    max: null,
                    step: null,
                    description: '',
                    enum: null,
                  },
                ],
              },
            ],
            peers_omitted: 0,
            variables_omitted: 0,
            operations: [],
          },
        },
      );

      const variable = rig.front.variable({ peer: 'Engine', name: 'engine.offset' });
      expect(variable.get()).toMatchObject({
        can_write: false,
        why: { code: 'scenario-running' },
      });
      expect(rig.front.scenario.get()?.handed_over).toBe(false);

      // Handover state arrives
      await rig.deliver(
        event(
          stateEvent(
            2,
            {
              paused: true,
              paused_at_ms: 500,
              pacing: 'not-paced',
              scenario: true,
              handed_over: true,
              scenario_over_at_ms: 500,
            },
            {
              pause: refusal('paused', 'already paused'),
              resume: ok,
              step: { ...ok, min_ms: 10, max_ms: 5000 },
              run_until: ok,
              speed: { ...ok, min: 0.5, max: 100, fastest: true },
              stop: ok,
              write: ok,
            },
          ),
        ),
      );

      expect(rig.front.status.get()).toMatchObject({ run: 'paused', result: null });
      expect(rig.front.clock.get().sim_ms).toBe(500);
      expect(rig.front.scenario.get()).toMatchObject({
        status: 'passed',
        handed_over: true,
        over_at_ms: 500,
      });
      expect(rig.front.controls.get().pause.reason?.code).toBe('paused');
      expect(rig.front.controls.get().resume.ok).toBe(true);
      expect(rig.front.controls.get().step.ok).toBe(true);
      expect(rig.front.controls.get().run_until.ok).toBe(true);
      expect(rig.front.controls.get().speed.ok).toBe(true);
      expect(rig.front.controls.get().stop.ok).toBe(true);
      expect(variable.get()).toMatchObject({ can_write: true, why: null, block: null });

      // Manual write + step succeed on the same run
      const writePromise = variable.write(25);
      const writeReq = rig.http.expectOne('api/write');
      expect(writeReq.request.body).toEqual({
        run_id: '1',
        instance: 'x',
        writes: { Engine: { 'engine.offset': 25 } },
      });
      writeReq.flush({ run_id: '1', operation: 1 });
      await rig.deliver({
        type: 'operation',
        run_id: '1',
        seq: 1,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 500, late: false, reason: null } },
      });
      await writePromise;

      const stepPromise = rig.front.time.step(100);
      const stepReq = rig.http.expectOne('api/time');
      const stepBody = stepReq.request.body as { id: number };
      expect(stepBody).toMatchObject({
        run_id: '1',
        command: 'step',
        ms: 100,
      });
      stepReq.flush({ instance: 'x', run_id: '1', id: stepBody.id });
      await rig.deliver(
        event(commandEvent(20, 'step', stepBody.id, { ms: 100, at_ms: 600 })),
        event(
          stateEvent(
            3,
            {
              paused: true,
              paused_at_ms: 600,
              pacing: 'not-paced',
              scenario: true,
              handed_over: true,
              scenario_over_at_ms: 500,
            },
            {
              pause: refusal('paused', 'already paused'),
              resume: ok,
              step: { ...ok, min_ms: 10, max_ms: 5000 },
              write: ok,
            },
          ),
        ),
      );
      await stepPromise;
      expect(rig.front.clock.get().sim_ms).toBe(600);

      // Stale session summary after live handover must not clear front.scenario.handed_over
      await rig.deliver({
        type: 'session',
        session: summary({
          speed: 'max',
          last_seq: 21,
          last_t_ms: 20,
          sim_ms: 600,
          launcher_state: stateEvent(
            1,
            { seq: 0, scenario: true, handed_over: false, scenario_over_at_ms: null },
            { write: refusal('scenario-running', 'scenario is running') },
          ),
          scenario: { ...scenarioSummary, handed_over: false },
        }),
      });
      expect(rig.front.scenario.get()).toMatchObject({
        status: 'passed',
        handed_over: true,
        over_at_ms: 500,
      });
      expect(variable.get()).toMatchObject({ can_write: true, why: null, block: null });
    });
  });

  describe('initial WebSocket sync wait and camelCase options', () => {
    it('awaits initial WS sync on stop(), forceStop(), time.*(), and variable.write() without 100 ms batching delay', async () => {
      const rig = await open();
      expect(rig.front.status.get().link).toBe('connecting');

      // Call stop() immediately after connect() while link === 'connecting'
      const stopPromise = rig.front.run.stop();
      rig.http.expectNone('api/stop-session');

      // Push hello + synced without advancing fake timers by BATCH_MS: immediate microtask flush!
      rig.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'inst-1',
        session: summary({}, '42'),
        allowed: serverAllowed(1, alreadyRunning),
        replay: null,
      });
      rig.socket.send(event(stateEvent(1), '42'));
      rig.socket.send({ type: 'synced', run_id: '42', last_seq: 11 });
      await Promise.resolve();
      await Promise.resolve();

      expect(rig.front.status.get().link).toBe('online');
      const stopReq = rig.http.expectOne('api/stop-session');
      expect(stopReq.request.body).toEqual({ force: false, run_id: '42', instance: 'inst-1' });
      stopReq.flush({ instance: 'inst-1', run_id: '42' });
      await expect(stopPromise).resolves.toBeUndefined();

      // Second client: forceStop() and time.pause() while link === 'connecting'
      const rig2 = await open();
      const pausePromise = rig2.front.time.pause();
      const forcePromise = rig2.front.run.forceStop();
      rig2.http.expectNone('api/time');
      rig2.http.expectNone('api/stop-session');

      rig2.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'inst-1',
        session: summary({}, '42'),
        allowed: serverAllowed(2, alreadyRunning),
        replay: null,
      });
      rig2.socket.send(event(stateEvent(2), '42'));
      rig2.socket.send({ type: 'synced', run_id: '42', last_seq: 12 });
      await Promise.resolve();
      await Promise.resolve();

      const timeReq = rig2.http.expectOne('api/time');
      const cmdId = (timeReq.request.body as { id: number }).id;
      expect(timeReq.request.body).toMatchObject({
        command: 'pause',
        run_id: '42',
        instance: 'inst-1',
      });
      timeReq.flush({ run_id: '42', command: { command: 'pause' } });
      // Command event also flushes on microtask without waiting BATCH_MS!
      rig2.socket.send(event(commandEvent(70, 'pause', cmdId), '42'));
      await expect(pausePromise).resolves.toBeUndefined();

      const forceReq = rig2.http.expectOne('api/stop-session');
      expect(forceReq.request.body).toEqual({ force: true, run_id: '42', instance: 'inst-1' });
      forceReq.flush({ instance: 'inst-1', run_id: '42' });
      await expect(forcePromise).resolves.toBeUndefined();

      // Third client: variable.write() while link === 'connecting' + immediate operation flush
      const rig3 = await open();
      const writePromise = rig3.front.variable('Engine.engine.offset').write(25);
      rig3.http.expectNone('api/write');
      rig3.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'inst-1',
        session: {
          ...summary({}, '42'),
          variables: {
            state: 'ready',
            reason: null,
            peers: [
              {
                name: 'Engine',
                connected: true,
                variables: [
                  {
                    name: 'engine.offset',
                    type: 'real',
                    role: 'input',
                    writable: true,
                    unit: 'rpm',
                    min: null,
                    max: null,
                    step: null,
                    description: '',
                    enum: null,
                  },
                ],
              },
            ],
            peers_omitted: 0,
            variables_omitted: 0,
            operations: [],
          },
        },
        allowed: serverAllowed(3, alreadyRunning),
        replay: null,
      });
      rig3.socket.send(event(stateEvent(3), '42'));
      rig3.socket.send({ type: 'synced', run_id: '42', last_seq: 13 });
      await Promise.resolve();
      await Promise.resolve();

      const writeReq = rig3.http.expectOne('api/write');
      expect(writeReq.request.body).toEqual({
        run_id: '42',
        instance: 'inst-1',
        writes: { Engine: { 'engine.offset': 25 } },
      });
      writeReq.flush({ run_id: '42', operation: 9 });
      rig3.socket.send({
        type: 'operation',
        run_id: '42',
        seq: 9,
        state: 'applied',
        done: true,
        peers: { Engine: { state: 'applied', applied_at_ms: 100, late: false, reason: null } },
      });
      await expect(writePromise).resolves.toEqual({
        session: '42',
        seq: 9,
        peer: 'Engine',
        name: 'engine.offset',
        value: 25,
        appliedAtMs: 100,
        late: false,
      });
    });

    it('rejects stop() and forceStop() with ActionError(not-running, refused) when no run is active after sync', async () => {
      // 1. Server has had no run yet (session: null)
      const rig = await open();
      const stopWhileConnecting = rig.front.run.stop().catch((e: unknown) => e as ActionError);
      rig.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'x',
        session: null,
        allowed: serverAllowed(1, startOk, notRunning, notRunning),
        replay: null,
      });
      rig.socket.send({ type: 'synced', run_id: null, last_seq: null });
      const err1 = await stopWhileConnecting;
      expect(err1).toBeInstanceOf(ActionError);
      expect(err1).toMatchObject({ code: 'not-running', outcome: 'refused' });
      rig.http.expectNone('api/stop-session');

      // Calling stop() or forceStop() again once already online with no run also rejects with not-running
      await expect(rig.front.run.stop()).rejects.toMatchObject({
        code: 'not-running',
        outcome: 'refused',
      });
      await expect(rig.front.run.forceStop()).rejects.toMatchObject({
        code: 'not-running',
        outcome: 'refused',
      });

      // 2. Previous run has already ended (session.state === 'ended', launcher not running)
      const rigEnded = await open();
      const stopEnded = rigEnded.front.run.stop().catch((e: unknown) => e as ActionError);
      rigEnded.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'x',
        session: summary({
          state: 'ended',
          launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
          allowed: serverAllowed(2, startOk, notRunning, notRunning),
        }),
        allowed: serverAllowed(2, startOk, notRunning, notRunning),
        replay: null,
      });
      rigEnded.socket.send({ type: 'synced', run_id: '1', last_seq: 10 });
      expect(await stopEnded).toMatchObject({ code: 'not-running', outcome: 'refused' });
      rigEnded.http.expectNone('api/stop-session');
    });

    it('deduplicates concurrent stop() and forceStop() calls made while connecting', async () => {
      const rig = await open();
      const s1 = rig.front.run.stop();
      const s2 = rig.front.run.stop();
      expect(s1).toBe(s2);

      rig.socket.send({
        type: 'hello',
        vehicle_sil_gui: 4,
        version: '0.1.0',
        instance: 'x',
        session: summary(),
        allowed: serverAllowed(1, alreadyRunning),
        replay: null,
      });
      rig.socket.send({ type: 'synced', run_id: '1', last_seq: 1 });
      await Promise.resolve();
      await Promise.resolve();

      // A third stop() while api/stop-session is in flight shares the same HTTP request
      const s3 = rig.front.run.stop();
      const req = rig.http.expectOne('api/stop-session');
      req.flush({ instance: 'x', run_id: '1' });
      await expect(Promise.all([s1, s2, s3])).resolves.toEqual([undefined, undefined, undefined]);
      rig.http.expectNone('api/stop-session');
    });

    it('never hangs on initial sync timeout, WS disconnect, auth rejection, protocol mismatch, or close()', async () => {
      // 1. Initial sync timeout (socket opens, never sends synced)
      const rigTimeout = await open();
      const stopTimeout = rigTimeout.front.run.stop().catch((e: unknown) => e as ActionError);
      const timeTimeout = rigTimeout.front.time.pause().catch((e: unknown) => e as ActionError);
      await vi.advanceTimersByTimeAsync(ANSWER_MS);
      expect(await stopTimeout).toMatchObject({ code: 'timeout', outcome: 'refused' });
      expect(await timeTimeout).toMatchObject({ code: 'timeout', outcome: 'refused' });

      // 2. WS disconnect during initial sync
      const rigOffline = await open();
      const stopOffline = rigOffline.front.run.stop().catch((e: unknown) => e as ActionError);
      const forceOffline = rigOffline.front.run.forceStop().catch((e: unknown) => e as ActionError);
      const timeOffline = rigOffline.front.time.pause().catch((e: unknown) => e as ActionError);
      rigOffline.socket.fail();
      expect(await stopOffline).toMatchObject({ code: 'offline', outcome: 'refused' });
      expect(await forceOffline).toMatchObject({ code: 'offline', outcome: 'refused' });
      expect(await timeOffline).toMatchObject({ code: 'offline', outcome: 'refused' });

      // 3. Auth rejection during initial sync
      const rigRejected = await open();
      const stopRejected = rigRejected.front.run.stop().catch((e: unknown) => e as ActionError);
      const timeRejected = rigRejected.front.time.pause().catch((e: unknown) => e as ActionError);
      rigRejected.socket.send({
        type: 'error',
        run_id: null,
        code: 'unauthorized',
        message: 'bad token',
        detail: null,
      });
      expect(await stopRejected).toMatchObject({ code: 'rejected', outcome: 'refused' });
      expect(await timeRejected).toMatchObject({ code: 'rejected', outcome: 'refused' });

      // 4. Protocol version mismatch during initial sync
      const rigIncompatible = await open();
      const stopIncompatible = rigIncompatible.front.run
        .stop()
        .catch((e: unknown) => e as ActionError);
      rigIncompatible.socket.send({
        type: 'hello',
        vehicle_sil_gui: 99,
        version: '9.0.0',
        instance: 'x',
        session: null,
        replay: null,
      });
      await vi.advanceTimersByTimeAsync(BATCH_MS);
      expect(await stopIncompatible).toMatchObject({ code: 'incompatible', outcome: 'refused' });

      // 5. close() while waiting for initial sync AND while api/stop-session is in flight
      const rigClosedPreSync = await open();
      const stopClosedPreSync = rigClosedPreSync.front.run
        .stop()
        .catch((e: unknown) => e as ActionError);
      const timeClosedPreSync = rigClosedPreSync.front.time
        .pause()
        .catch((e: unknown) => e as ActionError);
      rigClosedPreSync.front.close();
      expect(await stopClosedPreSync).toMatchObject({ code: 'closed', outcome: 'refused' });
      expect(await timeClosedPreSync).toMatchObject({ code: 'closed', outcome: 'refused' });

      const rigClosedMidHttp = await running();
      const stopClosedMidHttp = rigClosedMidHttp.front.run
        .stop()
        .catch((e: unknown) => e as ActionError);
      rigClosedMidHttp.http.expectOne('api/stop-session');
      rigClosedMidHttp.front.close();
      expect(await stopClosedMidHttp).toMatchObject({ code: 'closed', outcome: 'unknown' });
    });

    it('rejects unknown snake_case keys on connect(), createFront(), and until()', async () => {
      const http = new FakeHttp();
      const fake = new FakeWebSockets();

      await expect(
        connect({
          base_url: 'http://127.0.0.1:1/',
          http: http.transport,
          socket: fake.factory,
        } as unknown as Parameters<typeof connect>[0]),
      ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });

      await expect(
        connect({
          baseUrl: 'http://127.0.0.1:1/',
          answer_timeout_ms: 500,
          http: http.transport,
          socket: fake.factory,
        } as unknown as Parameters<typeof connect>[0]),
      ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });

      await expect(
        connect({
          baseUrl: 'http://127.0.0.1:1/',
          on_error: () => undefined,
          http: http.transport,
          socket: fake.factory,
        } as unknown as Parameters<typeof connect>[0]),
      ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });

      expect(http.requests).toHaveLength(0);
      expect(fake.sockets).toHaveLength(0);

      expect(() =>
        createFront({
          base_url: 'http://127.0.0.1:1/',
        } as unknown as Parameters<typeof createFront>[0]),
      ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));

      const rig = await running();
      // Even when the condition already holds, unknown keys like timeout_ms are rejected
      await expect(
        until(rig.front.status, (s) => s.run === 'running', {
          timeout_ms: 500,
        } as unknown as Parameters<typeof until>[2]),
      ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });

      await expect(
        until(rig.front.status, (s) => s.run === 'ended', { timeoutMs: -1 }),
      ).rejects.toMatchObject({ code: 'invalid-argument', outcome: 'refused' });
    });

    it('forwards case in check/start, exposes scenario case/params, and re-runs from session.intent in one call', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 4,
          version: '0.1.0',
          instance: 'x',
          session: summary({
            file: 'sub/simulation.yaml',
            state: 'ended',
            launcher: { pid: 1, running: false, exit_code: 0, signal: null, silent_s: null },
            allowed: serverAllowed(4, startOk, notRunning),
            intent: {
              scenario: 'sub/scenarios/cases.yaml',
              case: 'full_load',
              after_scenario: 'pause',
              variant: 'hil',
              params: { target_rpm: 3000, dry_run: false },
              connect: { Engine: 'lab' },
            },
            scenario: {
              file: 'sub/scenarios/cases.yaml',
              name: 'cases',
              case: 'full_load',
              params: [
                {
                  name: 'target_rpm',
                  value: 3000,
                  from: 'cli',
                  type: 'integer',
                  description: 'target rpm',
                },
              ],
              step_count: 1,
              steps: [],
              steps_omitted: 0,
              status: 'passed',
              over_at_ms: 200,
              timeline_url: 'api/timeline/1',
            },
          }),
          allowed: serverAllowed(4, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', run_id: '1', last_seq: 1 },
      );

      expect(rig.front.scenario.get()).toMatchObject({
        case: 'full_load',
        params: [
          {
            name: 'target_rpm',
            value: 3000,
            from: 'cli',
            type: 'integer',
            description: 'target rpm',
          },
        ],
      });

      // check() forwards case
      const checkPromise = rig.front.check(
        { scenario: 'sub/scenarios/cases.yaml', case: 'idle', variant: 'hil' },
        { file: 'sub/simulation.yaml' },
      );
      const checkReq = rig.http.expectOne('api/check');
      expect(checkReq.request.body).toEqual({
        file: 'sub/simulation.yaml',
        disable: [],
        scenario: 'sub/scenarios/cases.yaml',
        case: 'idle',
        variant: 'hil',
      });
      checkReq.flush({
        check: {
          vehicle_sil_check: 1,
          file: 'sub/simulation.yaml',
          result: 'passed',
          errors: 0,
          warnings: 0,
          notes: 0,
          participants: [],
          networks: [],
          findings: [],
          scenario: {
            file: 'sub/scenarios/cases.yaml',
            name: 'cases',
            case: 'idle',
            cases: ['idle', 'full_load'],
            steps: 1,
          },
        },
      });
      const checked = await checkPromise;
      expect(checked.scenario?.case).toBe('idle');

      // Re-run directly from session.intent in one call
      const session = rig.front.low.session.session.get();
      expect(session?.intent).toEqual({
        scenario: 'sub/scenarios/cases.yaml',
        case: 'full_load',
        after_scenario: 'pause',
        variant: 'hil',
        params: { target_rpm: 3000, dry_run: false },
        connect: { Engine: 'lab' },
      });
      const rerun = rig.front.run.start(session!.intent!, { file: session!.file });
      const runReq = rig.http.expectOne('api/run');
      expect(runReq.request.body).toEqual({
        file: 'sub/simulation.yaml',
        disable: [],
        scenario: 'sub/scenarios/cases.yaml',
        case: 'full_load',
        after_scenario: 'pause',
        variant: 'hil',
        params: { target_rpm: 3000, dry_run: false },
        connect: { Engine: 'lab' },
      });
      runReq.flush({ instance: 'x', run_id: '2' });
      await rig.deliver(
        {
          type: 'session',
          session: summary({ file: 'sub/simulation.yaml', state: 'running' }, '2'),
        },
        event(stateEvent(1), '2'),
      );
      await expect(rerun).resolves.toBeUndefined();
    });
  });

  describe('live watch and batch period options (WU-05)', () => {
    const varHello = (runId = '1') => ({
      type: 'hello',
      vehicle_sil_gui: 4,
      version: '0.1.0',
      instance: 'x',
      session: {
        id: runId,
        state: 'running',
        launcher: { pid: 1, running: true, exit_code: null, signal: null, silent_s: null },
        stop: 'none',
        variables: {
          state: 'ready',
          reason: null,
          peers: [
            {
              name: 'Engine',
              connected: true,
              variables: [
                {
                  name: 'engine.offset',
                  type: 'real',
                  role: 'input',
                  writable: true,
                  unit: 'rpm',
                  min: null,
                  max: null,
                  step: null,
                  description: '',
                  enum: null,
                },
                {
                  name: 'engine.speed',
                  type: 'real',
                  role: 'output',
                  writable: false,
                  unit: 'rpm',
                  min: null,
                  max: null,
                  step: null,
                  description: '',
                  enum: null,
                },
              ],
            },
          ],
          peers_omitted: 0,
          variables_omitted: 0,
          operations: [],
        },
      },
      replay: null,
    });

    const watchMessages = (socket: FakeSocket) =>
      socket.sent.filter((m) => {
        const type = (m as { type?: string }).type;
        return type === 'watch' || type === 'unwatch';
      });

    async function openWith(
      extra: Partial<Parameters<typeof connect>[0]> = {},
    ): Promise<Rig & { sockets: FakeWebSockets }> {
      const sockets = new FakeWebSockets();
      const http = new FakeHttp();
      const promise = connect({
        baseUrl: 'http://127.0.0.1:1/',
        token: 'secret',
        http: http.transport,
        socket: sockets.factory,
        answerTimeoutMs: ANSWER_MS,
        onError: (error) => {
          throw error;
        },
        ...extra,
      });
      http.expectOne('api/app').flush(app);
      const front = await promise;
      const socket = sockets.last();
      socket.open();
      const rig: Rig & { sockets: FakeWebSockets } = {
        front,
        http,
        socket,
        sockets,
        deliver: async (...messages) => {
          for (const message of messages) sockets.last().send(message);
          await vi.advanceTimersByTimeAsync(extra.batchMs ?? BATCH_MS);
        },
        timeBody: () => http.expectOne('api/time').request.body as ReturnType<Rig['timeBody']>,
        turn: async () => {
          await vi.advanceTimersByTimeAsync(0);
        },
      };
      rigs.push(rig);
      return rig;
    }

    it('rejects out-of-range, non-finite, non-number, and unknown watch/batch options on connect() and createFront()', async () => {
      const http = new FakeHttp();
      const fake = new FakeWebSockets();

      for (const bad of [
        { batchMs: -1 },
        { batchMs: 1001 },
        { batchMs: Number.NaN },
        { batchMs: Number.POSITIVE_INFINITY },
        { batchMs: '16' as unknown as number },
        { watchPeriodMs: 19 },
        { watchPeriodMs: 60_001 },
        { watchPeriodMs: Number.NaN },
        { watchPeriodMs: Number.POSITIVE_INFINITY },
        { watchPeriodMs: '20' as unknown as number },
      ]) {
        await expect(
          connect({
            baseUrl: 'http://127.0.0.1:1/',
            http: http.transport,
            socket: fake.factory,
            ...bad,
          }),
        ).rejects.toMatchObject({ code: 'invalid-argument', outcome: 'refused' });
        expect(() =>
          createFront({
            baseUrl: 'http://127.0.0.1:1/',
            http: http.transport,
            socket: fake.factory,
            ...bad,
          }),
        ).toThrow(expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }));
      }

      for (const unknown of [{ batch_ms: 16 }, { watch_period_ms: 20 }]) {
        await expect(
          connect({
            baseUrl: 'http://127.0.0.1:1/',
            http: http.transport,
            socket: fake.factory,
            ...unknown,
          } as unknown as Parameters<typeof connect>[0]),
        ).rejects.toMatchObject({ code: 'unknown-field', outcome: 'refused' });
      }
      expect(http.requests).toHaveLength(0);
      expect(fake.sockets).toHaveLength(0);
    });

    it('validates FrontVariable.subscribe options strictly without leaking watches on error', async () => {
      const rig = await openWith();
      await rig.deliver(varHello(), { type: 'synced', run_id: '1', last_seq: null });
      const variable = rig.front.variable('Engine.engine.offset');

      expect(() =>
        variable.subscribe(() => undefined, { period_ms: 20 } as unknown as { periodMs?: number }),
      ).toThrow(expect.objectContaining({ code: 'unknown-field', outcome: 'refused' }));

      for (const badOptions of [
        null,
        123,
        [],
        '20',
        { periodMs: 19 },
        { periodMs: 19.5 },
        { periodMs: 60_001 },
        { periodMs: 0 },
        { periodMs: -10 },
        { periodMs: Number.NaN },
        { periodMs: Number.POSITIVE_INFINITY },
        { periodMs: -Infinity },
        { periodMs: '20' },
        { periodMs: null },
      ]) {
        expect(() =>
          variable.subscribe(() => undefined, badOptions as unknown as { periodMs?: number }),
        ).toThrow(expect.objectContaining({ code: 'invalid-argument', outcome: 'refused' }));
      }
      await Promise.resolve();
      expect(watchMessages(rig.socket)).toEqual([]);
    });

    it('dynamically promotes a cached FrontVariable to shortest periodMs and restores slower period on unsubscribe', async () => {
      const rig = await openWith();
      await rig.deliver(varHello(), { type: 'synced', run_id: '1', last_seq: null });
      const offset = rig.front.variable('Engine.engine.offset');
      const speed = rig.front.variable('Engine.engine.speed');

      // 1) Initial subscribers at default 100 ms
      const slowValues: (number | boolean | null)[] = [];
      const stopSlow = offset.subscribe((v) => slowValues.push(v.value));
      const stopOther = speed.subscribe(() => undefined);
      await Promise.resolve();
      expect(watchMessages(rig.socket)).toEqual([
        {
          type: 'watch',
          id: 1,
          variables: [
            { peer: 'Engine', name: 'engine.offset' },
            { peer: 'Engine', name: 'engine.speed' },
          ],
          period_ms: 100,
        },
      ]);

      // 2) Second subscriber on the ALREADY cached FrontVariable requests { periodMs: 20 } -> promotes only offset to 20 ms!
      const fastValues: (number | boolean | null)[] = [];
      const stopFast = rig.front
        .variable('Engine.engine.offset')
        .subscribe((v) => fastValues.push(v.value), { periodMs: 20 });
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(1)).toEqual([
        {
          type: 'watch',
          id: 2,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 20,
        },
        {
          type: 'watch',
          id: 1,
          variables: [{ peer: 'Engine', name: 'engine.speed' }],
          period_ms: 100,
        },
      ]);

      // Both subscribers receive values delivered on the promoted 20 ms subscription
      await rig.deliver({
        type: 'values',
        id: 2,
        run_id: '1',
        sim_ms: 40,
        paused: false,
        peers: { Engine: { sample_ms: 40, values: { 'engine.offset': 42 } } },
      });
      expect(slowValues).toEqual([42]);
      expect(fastValues).toEqual([42]);

      // 3) Unsubscribing the fast subscriber restores offset back to 100 ms (and unwatches the 20 ms bucket)
      stopFast();
      stopFast(); // duplicate unsubscribe is a no-op
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(3)).toEqual([
        { type: 'unwatch', id: 2 },
        {
          type: 'watch',
          id: 1,
          variables: [
            { peer: 'Engine', name: 'engine.offset' },
            { peer: 'Engine', name: 'engine.speed' },
          ],
          period_ms: 100,
        },
      ]);

      // 4) Unsubscribing remaining subscribers drops all watches cleanly
      stopSlow();
      stopOther();
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(5)).toEqual([{ type: 'unwatch', id: 1 }]);
    });

    it('honors watchPeriodMs, slower-than-default periodMs, multi-tier step-down, and duplicate listener refs', async () => {
      const rig = await openWith({ watchPeriodMs: 500, batchMs: 16 });
      await rig.deliver(varHello(), { type: 'synced', run_id: '1', last_seq: null });
      const v = rig.front.variable('Engine.engine.offset');

      // Default subscriber uses watchPeriodMs (500 ms)
      const seen: (number | boolean | null)[] = [];
      const sharedListener = (view: { value: number | boolean | null }) => seen.push(view.value);
      const stopDefault = v.subscribe(sharedListener);
      await Promise.resolve();
      expect(watchMessages(rig.socket)).toEqual([
        {
          type: 'watch',
          id: 1,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 500,
        },
      ]);

      // Add duplicate listener reference at 75 ms (-> rounded to 50 ms) and another at 20 ms
      const stopMid = v.subscribe(sharedListener, { periodMs: 75 });
      const stopFast = v.subscribe(() => undefined, { periodMs: 20 });
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(1)).toEqual([
        { type: 'unwatch', id: 1 },
        {
          type: 'watch',
          id: 2,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 20,
        },
      ]);

      // Step down: remove 20 ms -> drops to 50 ms
      stopFast();
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(3)).toEqual([
        { type: 'unwatch', id: 2 },
        {
          type: 'watch',
          id: 3,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 50,
        },
      ]);

      // Duplicate listener reference subscribed twice receives value twice; unsubscribing mid leaves default active!
      rig.socket.send({
        type: 'values',
        id: 3,
        run_id: '1',
        sim_ms: 50,
        paused: false,
        peers: { Engine: { sample_ms: 50, values: { 'engine.offset': 11 } } },
      });
      await vi.advanceTimersByTimeAsync(15);
      expect(seen).toEqual([]); // batchMs: 16 has not elapsed yet
      await vi.advanceTimersByTimeAsync(1);
      expect(seen).toEqual([11, 11]);

      // Step down: remove 50 ms -> restores watchPeriodMs (500 ms)!
      stopMid();
      stopMid();
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(5)).toEqual([
        { type: 'unwatch', id: 3 },
        {
          type: 'watch',
          id: 4,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 500,
        },
      ]);

      stopDefault();
      await Promise.resolve();
      expect(watchMessages(rig.socket).slice(7)).toEqual([{ type: 'unwatch', id: 4 }]);

      // Even on a default-100 ms client, a sole subscriber asking for { periodMs: 500 } watches at 500 ms (not 100 ms)
      const defRig = await openWith();
      await defRig.deliver(varHello(), { type: 'synced', run_id: '1', last_seq: null });
      const stopSlowOnly = defRig.front
        .variable('Engine.engine.offset')
        .subscribe(() => undefined, { periodMs: 500 });
      await Promise.resolve();
      expect(watchMessages(defRig.socket)).toEqual([
        {
          type: 'watch',
          id: 1,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 500,
        },
      ]);
      stopSlowOnly();
      await Promise.resolve();
      expect(watchMessages(defRig.socket).slice(1)).toEqual([{ type: 'unwatch', id: 1 }]);
    });

    it('handles unsubscribe inside listener callback, run switch, reconnect, and batchMs: 0 cleanly', async () => {
      const rig = await openWith({ batchMs: 0 });
      rig.socket.send(varHello('1'));
      rig.socket.send({ type: 'synced', run_id: '1', last_seq: null });
      await Promise.resolve();

      const v = rig.front.variable('Engine.engine.offset');
      const slowSeen: (number | boolean | null)[] = [];
      const stopSlow = v.subscribe((view) => slowSeen.push(view.value));

      // Fast subscriber unsubscribes itself inside its listener callback on first value
      const fastSeen: (number | boolean | null)[] = [];
      let stopFast: () => void = () => undefined;
      stopFast = v.subscribe(
        (view) => {
          fastSeen.push(view.value);
          stopFast();
        },
        { periodMs: 20 },
      );
      await Promise.resolve();
      expect(watchMessages(rig.socket)).toEqual([
        {
          type: 'watch',
          id: 1,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 20,
        },
      ]);

      // batchMs: 0 delivers values on microtask without advancing timers; fast unsubscribes in callback -> restores 100 ms!
      rig.socket.send({
        type: 'values',
        id: 1,
        run_id: '1',
        sim_ms: 20,
        paused: false,
        peers: { Engine: { sample_ms: 20, values: { 'engine.offset': 7 } } },
      });
      await Promise.resolve();
      await Promise.resolve();
      expect(fastSeen).toEqual([7]);
      expect(slowSeen).toEqual([7]);
      expect(watchMessages(rig.socket).slice(1)).toEqual([
        { type: 'unwatch', id: 1 },
        {
          type: 'watch',
          id: 2,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 100,
        },
      ]);

      // Re-add a 20 ms subscriber, then test Run switch (1 -> 2) and WS reconnect: active shortest period (20 ms) persists!
      const stopFast2 = v.subscribe(() => undefined, { periodMs: 20 });
      await Promise.resolve();
      expect(watchMessages(rig.socket).at(-1)).toMatchObject({ type: 'watch', period_ms: 20 });

      // Run switch to run 2: new run's values arrive and watch stays at 20 ms
      rig.socket.send({ type: 'session', session: summary({}, '2') });
      rig.socket.send({
        type: 'variables',
        run_id: '2',
        variables: varHello('2').session.variables,
      });
      rig.socket.send({
        type: 'values',
        id: 3,
        run_id: '2',
        sim_ms: 10,
        paused: false,
        peers: { Engine: { sample_ms: 10, values: { 'engine.offset': 99 } } },
      });
      await Promise.resolve();
      expect(slowSeen.at(-1)).toBe(99);

      // WebSocket drops and reconnects: afterGreeting re-subscribes at the active shortest period (20 ms)!
      rig.socket.fail();
      await vi.advanceTimersByTimeAsync(500);
      const reconnected = rig.sockets.last();
      expect(reconnected).not.toBe(rig.socket);
      reconnected.open();
      expect(watchMessages(reconnected)).toEqual([
        {
          type: 'watch',
          id: 3,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 20,
        },
      ]);

      // Unsubscribing fast post-reconnect restores 100 ms on the reconnected socket!
      stopFast2();
      await Promise.resolve();
      expect(watchMessages(reconnected).slice(1)).toEqual([
        { type: 'unwatch', id: 3 },
        {
          type: 'watch',
          id: 4,
          variables: [{ peer: 'Engine', name: 'engine.offset' }],
          period_ms: 100,
        },
      ]);

      stopSlow();
      await Promise.resolve();
      expect(watchMessages(reconnected).slice(3)).toEqual([{ type: 'unwatch', id: 4 }]);
    });
  });
});
