// A small stand-in for the front door, for the tests of the parts: each state is a cell that the test sets, and each
// operation is a spy. The count of subscriptions shows whether a part cleaned up.
import type {
  CheckResult,
  Control,
  Front,
  FrontClock,
  FrontControls,
  FrontScenario,
  FrontStatus,
  FrontVariable,
  FrontWriteOptions,
  Outside,
  QueuedWrite,
  Readable,
  SessionView,
  VariableView,
  WriteResult,
} from '../index';

export class Cell<T> implements Readable<T> {
  readonly listeners = new Set<(value: T) => void>();
  constructor(private value: T) {}
  get = (): T => this.value;
  subscribe = (listener: (value: T) => void): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };
  set(value: T): void {
    this.value = value;
    for (const listener of [...this.listeners]) listener(value);
  }
  /** Replaces some fields. */
  patch(fields: Partial<T>): void {
    this.set({ ...this.value, ...fields });
  }
}

export const ok = (): Control => ({ ok: true, reason: null, caution: null });
export const refused = (code: string, message: string): Control => ({
  ok: false,
  reason: { code, message },
  caution: null,
});

export const viewOf = (fields: Partial<VariableView> = {}): VariableView =>
  ({
    value: 1,
    text: '1',
    unit: '',
    min: null,
    max: null,
    step: null,
    sim_ms: 0,
    stale: false,
    note: null,
    availability: 'active',
    can_write: true,
    why: null,
    writing: false,
    problem: null,
    condition: { kind: 'live', reading: { value: 1, sampleMs: 0, simMs: 0, paused: false } },
    block: null,
    write_status: { kind: 'idle' },
    ...fields,
  }) as VariableView;

export interface FakeVariable extends FrontVariable {
  cell: Cell<VariableView>;
  enqueue: ReturnType<
    typeof vi.fn<
      (value: number | boolean | string, options?: FrontWriteOptions) => Promise<QueuedWrite>
    >
  >;
  write: ReturnType<
    typeof vi.fn<
      (value: number | boolean | string, options?: FrontWriteOptions) => Promise<WriteResult>
    >
  >;
}

export interface FakeFront extends Front {
  cells: {
    status: Cell<FrontStatus>;
    clock: Cell<FrontClock>;
    controls: Cell<FrontControls>;
    scenario: Cell<FrontScenario | null>;
    session: Cell<SessionView | null>;
    outside: Cell<readonly Outside[]>;
  };
  variables: Map<string, FakeVariable>;
  spies: Record<
    | 'start'
    | 'stop'
    | 'forceStop'
    | 'pause'
    | 'resume'
    | 'step'
    | 'setSpeed'
    | 'runUntil'
    | 'scenarios'
    | 'connections'
    | 'check',
    ReturnType<typeof vi.fn>
  >;
  /** How many listeners the states and the variables have now. */
  subscriptions(): number;
}

export function fakeFront(): FakeFront {
  const status = new Cell<FrontStatus>({
    link: 'online',
    run: 'none',
    file: 'simulation.yaml',
    result: null,
    problem: null,
  });
  const clock = new Cell<FrontClock>({
    sim_ms: null,
    speed: null,
    actual_speed: null,
    pacing: 'not-paced',
  });
  const controls = new Cell<FrontControls>({
    start: ok(),
    stop: ok(),
    force_stop: refused('not-stopping', 'a first stop has not been sent'),
    pause: ok(),
    resume: ok(),
    step: { ...ok(), min_ms: 1, max_ms: 1000 },
    run_until: ok(),
    speed: { ...ok(), min: 0.1, max: 100, fastest: true },
    save: ok(),
  });
  const scenario = new Cell<FrontScenario | null>(null);
  const session = new Cell<SessionView | null>(null);
  const outside = new Cell<readonly Outside[]>([]);
  const variables = new Map<string, FakeVariable>();
  const spies = {
    start: vi.fn(async () => undefined),
    stop: vi.fn(async () => undefined),
    forceStop: vi.fn(async () => undefined),
    pause: vi.fn(async () => undefined),
    resume: vi.fn(async () => undefined),
    step: vi.fn(async () => undefined),
    setSpeed: vi.fn(async () => undefined),
    runUntil: vi.fn(async () => undefined),
    scenarios: vi.fn(async () => [{ path: 'scenarios/a.yaml', name: 'a.yaml' }]),
    connections: vi.fn(async () => []),
    check: vi.fn(async () => ({ findings: [] }) as unknown as CheckResult),
  };
  const front = {
    low: { session: { session } },
    status,
    clock,
    controls,
    scenario,
    outside,
    scenarios: spies.scenarios,
    connections: spies.connections,
    check: spies.check,
    run: { start: spies.start, stop: spies.stop, forceStop: spies.forceStop },
    time: {
      pause: spies.pause,
      resume: spies.resume,
      step: spies.step,
      setSpeed: spies.setSpeed,
      runUntil: spies.runUntil,
    },
    variable: (ref: string) => {
      let variable = variables.get(ref);
      if (!variable) {
        const cell = new Cell<VariableView>(viewOf());
        const dot = ref.indexOf('.');
        const peer = dot > 0 ? ref.slice(0, dot) : ref;
        const name = dot > 0 ? ref.slice(dot + 1) : ref;
        let nextSeq = 0;
        const makeResult = (value: number | boolean | string, seq: number): WriteResult => ({
          session: '1',
          seq,
          peer,
          name,
          value,
          appliedAtMs: seq * 10,
          late: false,
        });
        variable = {
          cell,
          get: cell.get,
          subscribe: cell.subscribe,
          enqueue: vi.fn(async (value: number | boolean | string): Promise<QueuedWrite> => {
            const seq = ++nextSeq;
            return {
              session: '1',
              seq,
              peer,
              name,
              value,
              applied: async () => makeResult(value, seq),
            };
          }),
          write: vi.fn(async (value: number | boolean | string) => makeResult(value, ++nextSeq)),
        };
        variables.set(ref, variable);
      }
      return variable;
    },
    close: vi.fn(),
    cells: { status, clock, controls, scenario, session, outside },
    variables,
    spies,
    subscriptions: () =>
      [
        status,
        clock,
        controls,
        scenario,
        session,
        outside,
        ...[...variables.values()].map((v) => v.cell),
      ].reduce((sum, cell) => sum + cell.listeners.size, 0),
  };
  return front as unknown as FakeFront;
}
