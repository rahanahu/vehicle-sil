import { ActionError } from '@vehicle-sil/gui-client';
import type { Front, QueuedWrite, VariableView, WriteResult } from '@vehicle-sil/gui-client';
import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';

export interface SmokeQueuedEvent {
  source: string;
  session: string;
  seq: number;
  peer: string;
  name: string;
  value: unknown;
}

export interface SmokeResultEvent {
  source: string;
  ok: boolean;
  peer: string;
  name: string;
  value: unknown;
  seq?: number;
  appliedAtMs?: number | null;
  late?: boolean;
  errorCode?: string;
  errorOutcome?: 'refused' | 'unknown';
  isActionError?: boolean;
}

export interface SmokeState {
  writes: { ref: string; value: unknown }[];
  queuedEvents: SmokeQueuedEvent[];
  resultEvents: SmokeResultEvent[];
  eventLog: string[];
  loads: { status: string; end: number; file: string | null }[];
  cursors: (number | null)[];
  ready: boolean;
  holdSliderEnqueue: () => void;
  releaseSliderEnqueue: () => void;
}

declare global {
  interface Window {
    __vsilSmoke?: SmokeState;
  }
}

export function initSmokeState(): SmokeState {
  const state: SmokeState = {
    writes: [],
    queuedEvents: [],
    resultEvents: [],
    eventLog: [],
    loads: [],
    cursors: [],
    ready: false,
    holdSliderEnqueue: () => undefined,
    releaseSliderEnqueue: () => undefined,
  };
  window.__vsilSmoke = state;
  return state;
}

export function recordQueued(smoke: SmokeState, source: string, detail: WriteQueuedInfo): void {
  smoke.queuedEvents.push({
    source,
    session: detail.session,
    seq: detail.seq,
    peer: detail.peer,
    name: detail.name,
    value: detail.value,
  });
  smoke.eventLog.push(`queued:${detail.seq}`);
}

export function recordResult(smoke: SmokeState, source: string, detail: WriteOutcome): void {
  if (detail.ok) {
    smoke.resultEvents.push({
      source,
      ok: true,
      peer: detail.peer,
      name: detail.name,
      value: detail.value,
      seq: detail.seq,
      appliedAtMs: detail.appliedAtMs,
      late: detail.late,
    });
    smoke.eventLog.push(`result:${detail.seq}`);
  } else {
    smoke.resultEvents.push({
      source,
      ok: false,
      peer: detail.peer,
      name: detail.name,
      value: detail.value,
      ...(detail.seq !== undefined ? { seq: detail.seq } : {}),
      errorCode: detail.error.code,
      errorOutcome: detail.error.outcome,
      isActionError: detail.error instanceof ActionError,
    });
    if (detail.seq !== undefined) smoke.eventLog.push(`result:${detail.seq}`);
  }
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
    condition: { kind: 'live', reading: { value: 1, sampleMs: 0, simMs: 0, paused: false } },
    block: null,
    write_status: { kind: 'idle' },
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
    'E.e.fault': { value: false, text: 'false' },
  };
  const variables = new Map<string, ReturnType<typeof makeCell<VariableView>>>();
  let nextSeq = 0;
  let sliderGate: Promise<void> | null = null;
  let releaseGate: (() => void) | null = null;

  smoke.holdSliderEnqueue = () => {
    sliderGate = new Promise<void>((resolve) => {
      releaseGate = resolve;
    });
  };
  smoke.releaseSliderEnqueue = () => {
    const release = releaseGate;
    sliderGate = null;
    releaseGate = null;
    release?.();
  };

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
      const dot = ref.indexOf('.');
      const peer = dot > 0 ? ref.slice(0, dot) : ref;
      const name = dot > 0 ? ref.slice(dot + 1) : ref;
      const enqueue = async (value: number | boolean | string): Promise<QueuedWrite> => {
        smoke.writes.push({ ref, value });
        if (ref === 'E.e.fault') {
          throw new ActionError('write-busy', 'too many writes', 'refused');
        }
        if (ref === 'E.e.throttle' && sliderGate !== null) {
          const gate = sliderGate;
          sliderGate = null;
          await gate;
        }
        const seq = ++nextSeq;
        const result: WriteResult = {
          session: 's1',
          seq,
          peer,
          name,
          value,
          appliedAtMs: 100 + seq * 10,
          late: false,
        };
        return {
          session: 's1',
          seq,
          peer,
          name,
          value,
          applied: async () => result,
        };
      };
      return {
        get: cell.get,
        subscribe: cell.subscribe,
        enqueue,
        write: (value: number | boolean | string) => enqueue(value).then((q) => q.applied()),
      };
    },
    close: () => undefined,
  };
  return front as unknown as Front;
}
