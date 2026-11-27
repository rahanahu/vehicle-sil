// One variable as the front door shows it: `client.variable('Engine.engine.throttle')`. The value state, whether it can
// be written and the write status are those of `client.low.variables.bind`; this adds native Promises for `enqueue` and
// `write`, a reason with a sentence for what cannot be written, and a note for a value that is not a live one.
import type { GuiClient } from './client';
import {
  ActionError,
  Control,
  FrontVariable,
  FrontWriteOptions,
  QueuedWrite,
  Reason,
  VariableView,
  WriteResult,
} from './front-types';
import { actionError } from './front-actions';
import { sameData } from './front-compose';
import type { BoundVariable, VariableBinding } from './variable-binding';
import type { ValueCondition, WriteBlock, WriteStatus, WriteValue } from './variable-state';
import { Readable, Writable } from './values';
import {
  InternalQueuedOperation,
  ON_SETTLED,
  OperationView,
  VariableRef,
  WATCH_PERIOD_MS_MAX,
  WATCH_PERIOD_MS_MIN,
  WriteError,
  WriteOptions,
  WriteWaitOptions,
  validateWriteOptions,
  validateWriteWaitOptions,
} from './variables';

const FRONT_WRITE_OPTION_FIELDS = new Set(['atMs', 'late', 'timeoutMs', 'keepalive', 'signal']);
const FRONT_SUBSCRIBE_OPTION_FIELDS = new Set(['periodMs']);

// `launcher` is not here: its sentence is the launcher's own (BoundVariable.launcherReason)
const WHY_BLOCKED: Record<Exclude<WriteBlock, 'launcher'>, string> = {
  'read-only': 'this variable is an output of the ECU: it cannot be written',
  disconnected: 'the connection to the GUI server is down: wait until it is back',
  'not-ready': 'the variables are not available now (no run, or the run has no Control Server)',
  ended: 'the run has ended: start a new one to write again',
  missing: 'this variable is not in the current run: check the names (ECU and variable)',
  'peer-disconnected': 'the ECU is not connected to the Control Server now',
  parameter: 'a parameter can be written only before the first step: start the run paused',
  'no-range': 'the variable has no range (min and max)',
  'no-value': 'there is no value yet',
};

/** The sentence for a value that is not a live one (null while it is live). */
export function noteOf(condition: ValueCondition): string | null {
  switch (condition.kind) {
    case 'live':
      return null;
    case 'paused':
      return 'the value at the paused time';
    case 'waiting':
      return 'no value has arrived yet';
    case 'missing':
      return 'this variable is not in the current run: check the names (ECU and variable)';
    case 'ended':
      return 'the last value of the run that has ended';
    case 'unavailable':
      return 'the variables are not available now (no run, or the run has no Control Server)';
    case 'stale':
      return {
        connection: 'the connection to the GUI server is down: the value may be out of date',
        reconnecting:
          'the server is connecting to the launcher again: the value may be out of date',
        peer: 'the ECU is not connected to the Control Server: the value may be out of date',
        reading: 'reading the values keeps failing: the value may be out of date',
      }[condition.why];
  }
}

function textOf(bound: BoundVariable): string {
  const value = bound.reading?.value;
  if (value === undefined || value === null) return '';
  const item = bound.info?.enum?.find((e) => e.value === value);
  return item ? item.name : String(value);
}

/** Split `"Peer.variable"` at the first dot (as the scenario files do); an object is taken as it is. */
export function refOf(ref: string | VariableRef): VariableRef {
  if (typeof ref !== 'string') return { peer: ref.peer, name: ref.name };
  const dot = ref.indexOf('.');
  if (dot <= 0 || dot === ref.length - 1) {
    throw new ActionError(
      'invalid-argument',
      `'${ref}' is not "ECU.variable": write the ECU's name, a dot, then the variable (or pass {peer, name} for an ECU whose name has a dot)`,
      'refused',
    );
  }
  return { peer: ref.slice(0, dot), name: ref.slice(dot + 1) };
}

export function validateFrontWriteOptions(options: unknown): ActionError | null {
  if (options === undefined) return null;
  if (typeof options !== 'object' || options === null || Array.isArray(options)) {
    return new ActionError('invalid-argument', 'the options of write must be an object', 'refused');
  }
  const candidate = options as Record<string, unknown>;
  const unknown = Object.keys(candidate).filter((k) => !FRONT_WRITE_OPTION_FIELDS.has(k));
  if (unknown.length > 0) {
    return new ActionError(
      'unknown-field',
      `the options of write have ${unknown.join(', ')}, which write does not know: the fields are ${[...FRONT_WRITE_OPTION_FIELDS].join(', ')}`,
      'refused',
    );
  }
  const err = validateWriteOptions(candidate);
  return err ? new ActionError(err.code ?? 'invalid-argument', err.message, 'refused') : null;
}

export function validateFrontWaitOptions(options: unknown): ActionError | null {
  const err = validateWriteWaitOptions(options);
  return err ? new ActionError(err.code ?? 'invalid-argument', err.message, 'refused') : null;
}

export function validateFrontSubscribeOptions(options: unknown): { periodMs?: number } {
  if (options === undefined) return {};
  if (typeof options !== 'object' || options === null || Array.isArray(options)) {
    throw new ActionError(
      'invalid-argument',
      'the options of subscribe must be an object',
      'refused',
    );
  }
  const candidate = options as Record<string, unknown>;
  const unknown = Object.keys(candidate).filter((k) => !FRONT_SUBSCRIBE_OPTION_FIELDS.has(k));
  if (unknown.length > 0) {
    throw new ActionError(
      'unknown-field',
      `the options of subscribe have ${unknown.join(', ')}, which subscribe does not know: the fields are ${[...FRONT_SUBSCRIBE_OPTION_FIELDS].join(', ')}`,
      'refused',
    );
  }
  const periodMs = candidate['periodMs'];
  if (
    periodMs !== undefined &&
    (typeof periodMs !== 'number' ||
      !Number.isFinite(periodMs) ||
      periodMs < WATCH_PERIOD_MS_MIN ||
      periodMs > WATCH_PERIOD_MS_MAX)
  ) {
    throw new ActionError(
      'invalid-argument',
      `options.periodMs must be a finite number of milliseconds between ${WATCH_PERIOD_MS_MIN} and ${WATCH_PERIOD_MS_MAX}`,
      'refused',
    );
  }
  return periodMs !== undefined ? { periodMs } : {};
}

interface Track {
  writing: boolean;
  problem: Reason | null;
}

function whyOf(bound: BoundVariable, launcher: Control): Reason | null {
  if (bound.block === null) return launcher.reason;
  // bind sets launcherReason whenever block is `launcher`
  if (bound.block === 'launcher') return bound.launcherReason ?? launcher.reason;
  return { code: bound.block, message: WHY_BLOCKED[bound.block] };
}

/**
 * `launcher` is what the launcher accepts for a write now (`state.allowed.write`, with the connection and the run). The
 * variable's own reason (it is an output, the ECU is not connected, ...) comes first; otherwise the launcher's, as
 * `bind` reports it (`block` `launcher`) or as `launcher` has it.
 */
export function viewOf(bound: BoundVariable, track: Track, launcher: Control): VariableView {
  const status = bound.writeStatus;
  const failed =
    status.kind === 'invalid' || status.kind === 'dropped' || 'code' in status
      ? ({
          code: 'code' in status && status.code ? status.code : status.kind,
          message: status.message,
        } as Reason)
      : null;
  return {
    value: bound.reading?.value ?? null,
    text: textOf(bound),
    unit: bound.info?.unit ?? '',
    min: bound.info?.min ?? null,
    max: bound.info?.max ?? null,
    step: bound.info?.step ?? null,
    sim_ms: bound.reading?.sampleMs ?? null,
    stale: bound.condition.kind === 'stale',
    note: noteOf(bound.condition),
    availability: bound.availability,
    can_write: bound.block === null && launcher.ok,
    why: whyOf(bound, launcher),
    writing:
      track.writing ||
      status.kind === 'sending' ||
      status.kind === 'queued' ||
      bound.pending !== null,
    problem: track.problem ?? failed,
    condition: bound.condition,
    block: bound.block,
    write_status: status,
  };
}

function failedOutcome(operation: OperationView | null): 'refused' | 'unknown' {
  if (!operation) return 'refused';
  if (operation.state === 'unknown' || operation.state === 'mixed') return 'unknown';
  for (const peer of Object.values(operation.peers)) {
    if (
      peer.state === 'unknown' ||
      peer.state === 'applied' ||
      (peer.appliedWrites !== null && peer.appliedWrites > 0)
    ) {
      return 'unknown';
    }
  }
  return 'refused';
}

/** A write that failed, as an ActionError (`no-answer` and `closed` may have been applied: `unknown`). */
export function writeFailure(error: unknown): ActionError {
  if (error instanceof ActionError) return error;
  if (error instanceof WriteError) {
    const outcome =
      error.kind === 'no-answer' || error.kind === 'closed'
        ? 'unknown'
        : error.kind === 'failed'
          ? failedOutcome(error.operation)
          : 'refused';
    return new ActionError(error.code ?? error.kind, error.message, outcome);
  }
  return actionError(error);
}

function toLowWriteOptions(options?: FrontWriteOptions): WriteOptions | undefined {
  if (!options) return undefined;
  const out: WriteOptions = {};
  if (options.atMs !== undefined) out.atMs = options.atMs;
  if (options.late !== undefined) out.late = options.late;
  if (options.timeoutMs !== undefined) out.timeoutMs = options.timeoutMs;
  if (options.keepalive !== undefined) out.keepalive = options.keepalive;
  if (options.signal !== undefined) out.signal = options.signal;
  return out;
}

interface ScopedProblem {
  scope: string;
  reason: Reason;
  binderStatusAtFailure: WriteStatus;
}

export function makeVariable(
  low: GuiClient,
  ref: VariableRef,
  /** Refuses a write the client must not send (the connection is refused, or the versions differ). */
  refuse: () => ActionError | null,
  /** What the launcher accepts for a write (see `viewOf`). */
  launcher: Readable<Control>,
): FrontVariable {
  const active = new Map<symbol, string>();
  let lastProblem: ScopedProblem | null = null;
  const track = new Writable<Track>({ writing: false, problem: null }, sameData);
  let binding: VariableBinding | null = null;
  let count = 0;
  let stops: (() => void)[] = [];

  const currentScope = (): string => {
    const vars = low.variables as {
      runEpoch?(): number;
      state: Readable<{ session: string | null }>;
    };
    return `${vars.runEpoch?.() ?? 0}:${vars.state.get().session ?? ''}`;
  };

  const currentBinderStatus = (): WriteStatus => {
    if (binding) return binding.state.get().writeStatus;
    const once = low.variables.bind(ref, { visible: false });
    try {
      return once.state.get().writeStatus;
    } finally {
      once.close();
    }
  };

  const computeTrack = (boundStatus?: WriteStatus): Track => {
    const scope = currentScope();
    for (const [token, tokenScope] of [...active]) {
      if (tokenScope !== scope) active.delete(token);
    }
    if (lastProblem) {
      if (lastProblem.scope !== scope) {
        lastProblem = null;
      } else if (
        boundStatus !== undefined &&
        boundStatus.kind === 'applied' &&
        boundStatus !== lastProblem.binderStatusAtFailure
      ) {
        lastProblem = null;
      }
    }
    return { writing: active.size > 0, problem: lastProblem?.reason ?? null };
  };

  const syncTrack = () => {
    track.set(computeTrack(binding?.state.get().writeStatus));
  };

  const setProblemForScope = (writeScope: string, failure: ActionError) => {
    if (currentScope() !== writeScope) return;
    lastProblem = {
      scope: writeScope,
      reason: { code: failure.code, message: failure.message },
      binderStatusAtFailure: currentBinderStatus(),
    };
  };

  const clearProblemForScope = (writeScope: string) => {
    if (lastProblem && lastProblem.scope === writeScope) {
      lastProblem = null;
    }
  };

  const view = new Writable<VariableView | null>(null, sameData);

  const refresh = (): VariableView | null => {
    if (!binding) return null;
    const bound = binding.state.get();
    const currentTrack = computeTrack(bound.writeStatus);
    track.set(currentTrack);
    const next = viewOf(bound, track.get(), launcher.get());
    if (!binding) return next;
    view.set(next);
    return view.get() ?? next;
  };

  /** The view without anyone subscribed: no values are asked for. The same object while nothing changed. */
  let peeked: VariableView | null = null;
  const peek = (): VariableView => {
    const once = low.variables.bind(ref, { visible: false });
    try {
      const bound = once.state.get();
      const currentTrack = computeTrack(bound.writeStatus);
      track.set(currentTrack);
      const next = viewOf(bound, track.get(), launcher.get());
      if (!peeked || !sameData(peeked, next)) peeked = next;
      return peeked;
    } finally {
      once.close();
    }
  };

  const get = (): VariableView => refresh() ?? peek();
  const subscribe: FrontVariable['subscribe'] = (listener, options) => {
    const { periodMs } = validateFrontSubscribeOptions(options);
    const subWatch = low.variables.watch([ref], periodMs !== undefined ? { periodMs } : undefined);
    if (count === 0) {
      try {
        binding = low.variables.bind(ref, { periodMs: WATCH_PERIOD_MS_MAX });
        stops = [
          binding.state.subscribe(refresh),
          low.variables.state.subscribe(refresh),
          track.subscribe(refresh),
          launcher.subscribe(refresh),
        ];
        count = 1;
        refresh();
      } catch (error) {
        for (const stop of stops) stop();
        stops = [];
        binding?.close();
        binding = null;
        count = 0;
        subWatch.close();
        throw error;
      }
    } else {
      count += 1;
    }
    const off = view.subscribe((v) => v && listener(v));
    let done = false;
    return () => {
      if (done) return;
      done = true;
      off();
      subWatch.close();
      if (--count === 0) {
        for (const stop of stops) stop();
        stops = [];
        binding?.close();
        binding = null;
        view.set(null);
      }
    };
  };

  const enqueue = (value: WriteValue, options?: FrontWriteOptions): Promise<QueuedWrite> => {
    const bad = validateFrontWriteOptions(options);
    if (bad) return Promise.reject(bad);
    if (options?.signal?.aborted) {
      return Promise.reject(
        new ActionError('aborted', 'the write was aborted before it was sent', 'refused'),
      );
    }
    const own = refuse();
    if (own) return Promise.reject(own);

    const writeScope = currentScope();
    const token = Symbol('write');
    active.set(token, writeScope);
    lastProblem = null;
    syncTrack();

    return low.variables
      .enqueue({ [ref.peer]: { [ref.name]: value } }, toLowWriteOptions(options))
      .then(
        (queuedOp): QueuedWrite => {
          const internal = queuedOp as InternalQueuedOperation;
          const hasSettledHook = typeof internal[ON_SETTLED] === 'function';
          internal[ON_SETTLED]?.((outcome) => {
            const removed = active.delete(token);
            if (!(outcome instanceof WriteError) && outcome.state === 'applied') {
              clearProblemForScope(writeScope);
            }
            if (removed || lastProblem === null) syncTrack();
          });
          return {
            session: queuedOp.session,
            seq: queuedOp.seq,
            peer: ref.peer,
            name: ref.name,
            value,
            applied: (waitOptions?: WriteWaitOptions): Promise<WriteResult> => {
              const badWait = validateFrontWaitOptions(waitOptions);
              if (badWait) return Promise.reject(badWait);
              return queuedOp.applied(waitOptions).then(
                (operation): WriteResult => {
                  if (!hasSettledHook) active.delete(token);
                  clearProblemForScope(writeScope);
                  syncTrack();
                  const peerResult = operation.peers[ref.peer];
                  return {
                    session: operation.session,
                    seq: operation.seq,
                    peer: ref.peer,
                    name: ref.name,
                    value,
                    appliedAtMs: peerResult?.appliedAtMs ?? null,
                    late: peerResult?.late ?? false,
                  };
                },
                (error: unknown) => {
                  const failure = writeFailure(error);
                  if (!hasSettledHook) active.delete(token);
                  setProblemForScope(writeScope, failure);
                  syncTrack();
                  throw failure;
                },
              );
            },
          };
        },
        (error: unknown) => {
          const failure = writeFailure(error);
          active.delete(token);
          setProblemForScope(writeScope, failure);
          syncTrack();
          throw failure;
        },
      );
  };

  return {
    get,
    subscribe,
    enqueue,
    write: (value: WriteValue, options?: FrontWriteOptions): Promise<WriteResult> =>
      enqueue(value, options).then((queued) =>
        queued.applied({
          ...(options?.timeoutMs !== undefined ? { timeoutMs: options.timeoutMs } : {}),
          ...(options?.signal !== undefined ? { signal: options.signal } : {}),
        }),
      ),
  };
}
