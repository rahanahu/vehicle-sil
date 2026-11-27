import type { Front, VariableView } from '@vehicle-sil/gui-client';

export interface SmokeState {
  writes: { ref: string; value: unknown }[];
  loads: { status: string; end: number; file: string | null }[];
  cursors: (number | null)[];
  ready: boolean;
}

declare global {
  interface Window {
    __vsilSmoke?: SmokeState;
  }
}

export function initSmokeState(): SmokeState {
  const state: SmokeState = { writes: [], loads: [], cursors: [], ready: false };
  window.__vsilSmoke = state;
  return state;
}

export const SAMPLE_TIMELINE = {
  vehicle_sil_timeline: 1,
  vehicle_sil: '0.5.0',
  stage: 'run',
  file: 'simulation.yaml',
  variant: null,
  params: [],
  time_step_ms: 10,
  scenario: {
    file: 'scenario.yaml',
    name: 'smoke',
    status: 'passed',
    over_at_ms: 8020,
    recorded_to_ms: 8010,
    end_ms: null,
  },
  variables: [
    {
      name: 'Body.level',
      participant: 'Body',
      type: 'real',
      causality: 'input',
      unit: '',
      min: 0,
      max: 100,
      items: [],
      ecu_step_ms: 10,
      written: true,
      watched: false,
    },
  ],
  steps: [
    {
      index: 1,
      line: 7,
      kind: 'set',
      text: 'set Body.level = 5',
      status: 'passed',
      start_ms: 0,
      at_ms: 0,
      end_ms: 0,
      code: null,
      message: null,
      lines: [0],
      watch: null,
      failed: null,
    },
  ],
  series: [
    {
      variable: 'Body.level',
      source: 'written',
      encoding: 'all',
      columns: ['at_ms', 'value'],
      samples: null,
      first_ms: null,
      last_ms: null,
      not_applied_from: null,
      points: [
        [0, 5],
        [4000, 40],
        [8010, 81],
      ],
    },
  ],
  lines: [
    {
      variable: 'Body.level',
      kind: 'set',
      line: 7,
      step: 1,
      from_ms: 0,
      to_ms: 0,
      base: 5,
      base_source: 'written',
      shape: null,
      text: 'Body.level = 5',
    },
  ],
  findings: [],
  limits: {
    points_per_series: 1000,
    points_total: 1000000,
    dropped: [],
  },
};

function makeCell<T>(initial: T) {
  let current = initial;
  const listeners = new Set<(value: T) => void>();
  return {
    get: () => current,
    subscribe: (fn: (value: T) => void) => {
      listeners.add(fn);
      return () => {
        listeners.delete(fn);
      };
    },
    set: (next: T) => {
      current = next;
      for (const fn of [...listeners]) fn(next);
    },
  };
}

function makeView(fields: Partial<VariableView>): VariableView {
  return {
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
    condition: 'live',
    block: null,
    write_status: 'idle',
    ...fields,
  } as VariableView;
}

export function createSmokeFront(smoke: SmokeState): Front {
  const ok = { ok: true, reason: null, caution: null };
  const status = makeCell({
    link: 'online' as const,
    run: 'running' as const,
    file: 'simulation.yaml',
    result: null,
    problem: null,
  });
  const clock = makeCell({
    sim_ms: 100,
    speed: 1 as number | 'max' | null,
    actual_speed: 1,
    pacing: 'on-time' as const,
  });
  const controls = makeCell({
    start: ok,
    stop: ok,
    force_stop: {
      ok: false,
      reason: { code: 'not-stopping', message: 'not stopping' },
      caution: null,
    },
    pause: ok,
    resume: ok,
    step: { ...ok, min_ms: 1, max_ms: 1000 },
    run_until: ok,
    speed: { ...ok, min: 0.1, max: 100, fastest: true },
    save: ok,
  });
  const scenario = makeCell(null);
  const outside = makeCell([]);

  const initialViews: Record<string, Partial<VariableView>> = {
    'E.e.flag': { value: true, text: 'true' },
    'E.e.throttle': { value: 50, text: '50', unit: '%', min: 0, max: 100, step: 1 },
    'E.e.speed': { value: 42, text: '42', unit: 'km/h' },
  };
  const variables = new Map<string, ReturnType<typeof makeCell<VariableView>>>();

  const front = {
    low: {},
    status,
    clock,
    controls,
    scenario,
    outside,
    scenarios: async () => [],
    connections: async () => [],
    check: async () => ({ findings: [] }),
    run: {
      start: async () => undefined,
      stop: async () => undefined,
      forceStop: async () => undefined,
    },
    time: {
      pause: async () => undefined,
      resume: async () => undefined,
      step: async () => undefined,
      setSpeed: async () => undefined,
      runUntil: async () => undefined,
    },
    variable: (ref: string) => {
      let cell = variables.get(ref);
      if (!cell) {
        cell = makeCell(makeView(initialViews[ref] ?? {}));
        variables.set(ref, cell);
      }
      return {
        get: cell.get,
        subscribe: cell.subscribe,
        write: async (value: number | boolean | string) => {
          smoke.writes.push({ ref, value });
        },
      };
    },
    close: () => undefined,
  };
  return front as unknown as Front;
}
