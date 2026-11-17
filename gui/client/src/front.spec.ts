import { BATCH_MS } from './connection';
import { ActionError, Front } from './front-types';
import { connect, until, watch } from './front';
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
  holding: false,
  speed: 1,
  pacing: 'on-time',
  stop: { requests: 0, by: null },
  scenario: false,
  allowed: launcherAllowed(allowed),
  outside: [],
  ...more,
});

const pausedState = (rev: number) =>
  stateEvent(
    rev,
    { paused: true },
    { pause: ok, resume: ok, step: { ...ok, min_ms: 10, max_ms: 5000 } },
  );

const timeEvent = (seq: number, simMs: number, paused: boolean) => ({
  event: 'time',
  seq,
  t_ms: seq,
  sim_ms: simMs,
  speed: 1,
  paused,
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

const serverAllowed = (rev: number, start: object, stop: object = ok) => ({
  rev,
  start,
  stop,
  force: stop,
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

const summary = (more: Record<string, unknown> = {}, id = 1) => ({
  id,
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
const event = (e: unknown, session = 1) => ({
  type: 'event',
  session,
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

  async function open(features: string[] = []): Promise<Rig> {
    const fake = new FakeWebSockets();
    const http = new FakeHttp();
    const promise = connect({
      base_url: 'http://127.0.0.1:1/',
      token: 'secret',
      http: http.transport,
      socket: fake.factory,
      answer_timeout_ms: ANSWER_MS,
      on_error: (error) => {
        throw error;
      },
    });
    const answer = structuredClone(app) as { launcher: { version: { features: string[] } } };
    answer.launcher.version.features.push(...features);
    http.expectOne('api/app').flush(answer);
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
        vehicle_sil_gui: 1,
        version: '0.1.0',
        instance: 'x',
        session: summary(),
        allowed: serverAllowed(3, alreadyRunning),
        replay: null,
      },
      event(state),
      event(timeEvent(30, 500, (state as { paused: boolean }).paused)),
      { type: 'synced', session: 1, last_seq: 40 },
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
      expect(c.speed).toMatchObject({ ok: true, min: 0.5, max: 100 });
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
    });

    it('keeps the same object while nothing changed', async () => {
      const rig = await running();
      const before = [rig.front.status.get(), rig.front.controls.get(), rig.front.clock.get()];
      await rig.deliver(event(timeEvent(31, 500, false)));
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
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', session: null, last_seq: null },
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
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', session: null, last_seq: null },
      );
      expect(rig.front.controls.get().start.ok).toBe(true);
      await rig.deliver({ type: 'allowed', session: null, allowed: serverAllowed(2, starting) });
      expect(rig.front.controls.get().start).toEqual({
        ok: false,
        reason: { code: 'starting', message: starting.message },
        caution: null,
      });
      expect(rig.front.status.get().run).toBe('starting');
      // stop is allowed while a start is on its way, as the server says
      expect(rig.front.controls.get().stop.ok).toBe(true);
      await rig.deliver({
        type: 'allowed',
        session: null,
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

    it('does not let an older allowed replace a newer one', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(5, startOk, notRunning),
          replay: null,
        },
        { type: 'allowed', session: null, allowed: serverAllowed(4, alreadyRunning) },
        { type: 'synced', session: null, last_seq: null },
      );
      expect(rig.front.controls.get().start.ok).toBe(true);
    });

    it('calls the run stopping, with no result, while the ended phase is in but the result is not', async () => {
      const rig = await running();
      await rig.deliver(event(stateEvent(2, { phase: 'ended' })));
      expect(rig.front.status.get()).toMatchObject({ run: 'stopping', result: null });
    });

    it('shows the sentences of a result by its name, and a stop by the operator is not a failure', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 1,
            result: 'interrupted',
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
            launcher: { pid: 1, running: false, exit_code: 130, signal: null, silent_s: null },
            allowed: serverAllowed(4, startOk, notRunning),
          }),
        },
      );
      const status = rig.front.status.get();
      expect(status.run).toBe('ended');
      expect(status.result).toMatchObject({ end: 'stopped', ok: true, scenario: null });
      expect(status.problem).toBeNull();
      expect(rig.front.controls.get().start.ok).toBe(true);
    });

    it('shows a failed run as the problem, and the launcher dying without a result as crashed', async () => {
      const rig = await running();
      await rig.deliver(
        event({
          event: 'result',
          seq: 50,
          t_ms: 50,
          result: {
            vehicle_sil_run: 1,
            result: 'failed',
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
        message: 'the run failed: Engine failed',
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
        session: null,
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
    it('ties the answer to the command by its id, and ignores the same name with another id', async () => {
      const rig = await running();
      let done = false;
      const pause = rig.front.time.pause().then(() => (done = true));
      const body = rig.timeBody();
      expect(body).toMatchObject({ command: 'pause' });
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

    it('sends what the controls refuse when the copy cannot be trusted, and lets the launcher answer', async () => {
      // No run in the copy: the operation goes out, and the server's refusal is the answer
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', session: null, last_seq: null },
      );
      expect(rig.front.controls.get().step.ok).toBe(false);
      const step = rig.front.time.step(10).catch((e: unknown) => e as ActionError);
      rig.http
        .expectOne('api/time')
        .flush(
          { error: { code: 'not-running', message: 'no simulation is running', detail: null } },
          { status: 409, statusText: 'Conflict' },
        );
      expect(await step).toMatchObject({ code: 'not-running', outcome: 'refused' });
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
      await rig.deliver(event(timeEvent(61, 520, false)));
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(62, 530, true)));
      expect(done).toBe(false);
      await rig.deliver(event(pausedState(3)));
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
      await rig.deliver(event(timeEvent(62, 650, true)), event(pausedState(3)));
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
        event(timeEvent(60, 540, true)),
        event(commandEvent(61, 'step', id, { ms: 10, at_ms: 550 })),
      );
      expect(done).toBe(false);
      await rig.deliver(event(timeEvent(62, 550, true)), event(pausedState(3)));
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
      await rig.deliver(event(timeEvent(61, 900, true)), event(pausedState(3)));
      await until;
      expect(done).toBe(true);
      // the controls say it cannot be sent unless the launcher has stdin-run-until
      expect(rig.front.controls.get().run_until).toMatchObject({
        ok: false,
        reason: { code: 'unsupported' },
      });
    });

    it('turns run_until on when the launcher has the feature stdin-run-until', async () => {
      const rig = await open(['stdin-run-until']);
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: summary(),
          allowed: serverAllowed(3, alreadyRunning),
          replay: null,
        },
        event(pausedState(1)),
        { type: 'synced', session: 1, last_seq: 40 },
      );
      expect(rig.front.controls.get().run_until.ok).toBe(true);
    });

    it('refuses a second start while the first is on its way, and a start with the token refused', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', session: null, last_seq: null },
      );
      void rig.front.run.start(undefined, { file: 'sim.yaml' }).catch(() => undefined);
      await expect(rig.front.run.start(undefined, { file: 'sim.yaml' })).rejects.toMatchObject({
        code: 'busy',
        outcome: 'refused',
      });
      expect(rig.http.requests.filter((r) => r.request.path === 'api/run')).toHaveLength(1);
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
      rig.http.expectOne('api/stop').flush({ session: summary({ stop: 'requested' }) });
      await Promise.all([first, second]);
      rig.http.expectNone('api/stop');
    });

    it('turns force_stop on after a stop has been asked for', async () => {
      const rig = await running();
      const stop = rig.front.run.stop();
      expect(rig.front.controls.get().stop.reason?.code).toBe('busy');
      rig.http.expectOne('api/stop').flush({ session: summary({ stop: 'requested' }) });
      await stop;
      expect(rig.front.status.get().run).toBe('stopping');
      expect(rig.front.controls.get().force_stop.ok).toBe(true);
      expect(rig.front.controls.get().stop.reason?.code).toBe('already-stopping');
    });

    it('starts the run with what it was given, and says starting meanwhile', async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: null,
          allowed: serverAllowed(1, startOk, notRunning),
          replay: null,
        },
        { type: 'synced', session: null, last_seq: null },
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
      request.flush({ session: summary() });
      await start;
      expect(rig.front.status.get().run).not.toBe('none');
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

  /** A run with the variable Engine.engine.offset, and the launcher's state `state`. */
  async function withVariable(state: unknown = stateEvent(1)): Promise<Rig> {
    const rig = await open();
    await rig.deliver(
      {
        type: 'hello',
        vehicle_sil_gui: 1,
        version: '0.1.0',
        instance: 'x',
        session: {
          id: 1,
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
      { type: 'synced', session: 1, last_seq: null },
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
      request.flush({ session_id: 1, operation: 4 });
      await rig.deliver({
        type: 'operation',
        session: 1,
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

    it("shows a refused write in the view and rejects with the server's code", async () => {
      const rig = await open();
      await rig.deliver(
        {
          type: 'hello',
          vehicle_sil_gui: 1,
          version: '0.1.0',
          instance: 'x',
          session: summary(),
          allowed: serverAllowed(3, alreadyRunning),
          replay: null,
        },
        { type: 'synced', session: 1, last_seq: null },
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
  });

  describe('watch and until', () => {
    it('calls now and on each change, for one source or several, and stops', async () => {
      const rig = await running();
      const seen: string[] = [];
      const stop = watch([rig.front.status, rig.front.clock], (status, clock) =>
        seen.push(`${status.run}@${clock.sim_ms}`),
      );
      expect(seen).toEqual(['running@500']);
      await rig.deliver(event(timeEvent(31, 600, false)));
      expect(seen.at(-1)).toBe('running@600');
      stop();
      const count = seen.length;
      await rig.deliver(event(timeEvent(32, 700, false)));
      expect(seen).toHaveLength(count);
    });

    it('resolves with the first value that holds, and rejects on a time-out and on an abort', async () => {
      const rig = await running();
      expect(await until(rig.front.status, (s) => s.run === 'running')).toMatchObject({
        run: 'running',
      });
      const waiting = until(rig.front.status, (s) => s.run === 'ended', { timeout_ms: 500 }).catch(
        (e: unknown) => e as ActionError,
      );
      await vi.advanceTimersByTimeAsync(500);
      expect(await waiting).toMatchObject({ code: 'timeout', outcome: 'unknown' });
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
        base_url: 'http://127.0.0.1:1/',
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
  });
});
