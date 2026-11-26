// One variable as the front door shows it: `client.variable('Engine.engine.throttle')`. The value state, whether it can
// be written and the write status are those of `client.low.variables.bind`; this adds a Promise for a write, a reason
// with a sentence for what cannot be written, and a note for a value that is not a live one.
import type { GuiClient } from './client';
import { ActionError, Control, FrontVariable, Reason, VariableView } from './front-types';
import { actionError } from './front-actions';
import { sameData } from './front-compose';
import type { BoundVariable, VariableBinding } from './variable-binding';
import type { ValueCondition, WriteBlock, WriteValue } from './variable-state';
import { Readable, Writable } from './values';
import { VariableRef, WriteError } from './variables';

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

/** A write that failed, as an ActionError (`no-answer` and `closed` may have been applied: `unknown`). */
export function writeFailure(error: unknown): ActionError {
  if (error instanceof WriteError) {
    return new ActionError(
      error.code ?? error.kind,
      error.message,
      error.kind === 'no-answer' || error.kind === 'closed' ? 'unknown' : 'refused',
    );
  }
  return actionError(error);
}

export function makeVariable(
  low: GuiClient,
  ref: VariableRef,
  /** Refuses a write the client must not send (the connection is refused, or the versions differ). */
  refuse: () => ActionError | null,
  /** What the launcher accepts for a write (see `viewOf`). */
  launcher: Readable<Control>,
): FrontVariable {
  const track = new Writable<Track>({ writing: false, problem: null }, sameData);
  const view = new Writable<VariableView | null>(null, sameData);
  let binding: VariableBinding | null = null;
  let count = 0;
  let stops: (() => void)[] = [];

  const refresh = () => {
    if (binding) view.set(viewOf(binding.state.get(), track.get(), launcher.get()));
  };

  /** The view without anyone subscribed: no values are asked for. The same object while nothing changed. */
  let peeked: VariableView | null = null;
  const peek = (): VariableView => {
    const once = low.variables.bind(ref, { visible: false });
    try {
      const next = viewOf(once.state.get(), track.get(), launcher.get());
      if (!peeked || !sameData(peeked, next)) peeked = next;
      return peeked;
    } finally {
      once.close();
    }
  };

  const readable: Readable<VariableView> = {
    get: () => {
      if (!binding) return peek();
      return view.get() as VariableView;
    },
    subscribe: (listener) => {
      if (count++ === 0) {
        binding = low.variables.bind(ref);
        stops = [
          binding.state.subscribe(refresh),
          track.subscribe(refresh),
          launcher.subscribe(refresh),
        ];
        refresh();
      }
      const off = view.subscribe((v) => v && listener(v));
      let done = false;
      return () => {
        if (done) return;
        done = true;
        off();
        if (--count === 0) {
          for (const stop of stops) stop();
          stops = [];
          binding?.close();
          binding = null;
          view.set(null);
        }
      };
    },
  };

  return {
    get: readable.get,
    subscribe: readable.subscribe,
    write: (value: WriteValue, options?: { keepalive?: boolean }) => {
      const own = refuse();
      if (own) return Promise.reject(own);
      track.set({ writing: true, problem: null });
      return low.variables
        .write(
          { [ref.peer]: { [ref.name]: value } },
          options?.keepalive !== undefined ? { keepalive: options.keepalive } : undefined,
        )
        .then(
          () => {
            track.set({ writing: false, problem: null });
          },
          (error: unknown) => {
            const failure = writeFailure(error);
            track.set({
              writing: false,
              problem: { code: failure.code, message: failure.message },
            });
            throw failure;
          },
        );
    },
  };
}
