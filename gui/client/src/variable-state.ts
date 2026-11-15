import type { VariableInfo, VariableReading, VariablesStatus } from './variables';

// Pure functions that decide the current state of one variable, and their shapes. A screen widget (slider, lamp and
// so on) reads these shapes to know how to show the value, whether it can write, and what became of a write.
// client.variables.bind (variable-binding.ts) puts them together.

/**
 * State of the value (decided by valueCondition).
 * - unavailable: the variable API is unavailable (not running, this run has no variables, connecting, cannot connect).
 * - missing: the variable is not in the current run.
 * - waiting: in the current run, but no value has arrived yet.
 * - live: the last value while running.
 * - paused: the value at the paused time (includes the values of steps that began before that time).
 * - stale: a value that may be out of date (why: the screen lost its connection to the server, the server is
 *   reconnecting to the launcher, the ECU disconnected, or reading values keeps failing).
 * - ended: the last value read in a run that has ended.
 */
export type ValueCondition =
  | { kind: 'unavailable'; status: VariablesStatus }
  | { kind: 'missing' }
  | { kind: 'waiting' }
  | { kind: 'live'; reading: VariableReading }
  | { kind: 'paused'; reading: VariableReading }
  | {
      kind: 'stale';
      reading: VariableReading;
      why: 'connection' | 'reconnecting' | 'peer' | 'reading';
    }
  | { kind: 'ended'; reading: VariableReading };

/** Input of valueCondition. */
export interface ConditionInput {
  /** State of the variable API (the status of client.variables.state). */
  status: VariablesStatus;
  /** Whether the variable is in the list of the current run. */
  known: boolean;
  /** The last value (undefined if none). */
  reading: VariableReading | undefined;
  /** Whether the connection between the client and the GUI server is open. */
  connected: boolean;
  /** Whether that ECU is connected to the Control Server. */
  peerConnected: boolean;
  /**
   * Reading values keeps failing (the status stays ready; the summary's reason says why). The values received are the
   * last ones read.
   */
  readFailing?: boolean;
}

/** Decides the state of the value. */
export function valueCondition(input: ConditionInput): ValueCondition {
  const { status, known, reading } = input;
  // Unless there is a list (ready, ended, or connecting while reconnecting), even the variable's name is not certain
  if (!known) {
    return status === 'ready' || status === 'ended'
      ? { kind: 'missing' }
      : { kind: 'unavailable', status };
  }
  if (reading === undefined) {
    return status === 'ready' || status === 'ended'
      ? { kind: 'waiting' }
      : { kind: 'unavailable', status };
  }
  if (!input.connected) return { kind: 'stale', reading, why: 'connection' };
  if (status === 'ended') return { kind: 'ended', reading };
  if (status !== 'ready') return { kind: 'stale', reading, why: 'reconnecting' };
  if (!input.peerConnected) return { kind: 'stale', reading, why: 'peer' };
  if (input.readFailing) return { kind: 'stale', reading, why: 'reading' };
  return reading.paused ? { kind: 'paused', reading } : { kind: 'live', reading };
}

/** Whether two value states are the same (kind, value, reason). */
export function sameCondition(a: ValueCondition, b: ValueCondition): boolean {
  if (a.kind !== b.kind) return false;
  const reading = (c: ValueCondition) => ('reading' in c ? c.reading : undefined);
  const status = (c: ValueCondition) => (c.kind === 'unavailable' ? c.status : undefined);
  const why = (c: ValueCondition) => (c.kind === 'stale' ? c.why : undefined);
  return reading(a) === reading(b) && status(a) === status(b) && why(a) === why(b);
}

/**
 * Why the variable cannot be written now (null if it can).
 * - read-only: a read-only variable (role output).
 * - disconnected: the connection between the client and the GUI server is down.
 * - not-ready: the variable API is unavailable (not running, connecting, cannot connect).
 * - ended: the run has ended.
 * - missing: the variable is not in the current run.
 * - peer-disconnected: that ECU is not connected to the Control Server.
 * - parameter: a parameter can only be written before the first step.
 * - scenario: a scenario is running (watch only; the launcher refuses writes).
 * - no-range: a widget that needs a range (slider) is bound to a variable without one (min and max)
 *   (BindOptions.needsRange).
 * - no-value: there is no value yet, so a widget that moves from the value (slider) cannot move
 *   (BindOptions.needsValue).
 */
export type WriteBlock =
  | 'read-only'
  | 'disconnected'
  | 'not-ready'
  | 'ended'
  | 'missing'
  | 'peer-disconnected'
  | 'parameter'
  | 'no-range'
  | 'no-value'
  | 'scenario';

/** Input of writeBlock. */
export interface WriteBlockInput {
  info: VariableInfo | undefined;
  status: VariablesStatus;
  connected: boolean;
  /** Whether a run started paused is waiting before the first step (beforeStart of client.time.state). */
  beforeStart: boolean;
  /** Whether that ECU is connected to the Control Server. */
  peerConnected: boolean;
}

/**
 * Decides why the variable cannot be written now (null if it can). Only the reasons that do not depend on the widget:
 * client.variables.bind adds scenario, no-range and no-value.
 */
export function writeBlock(input: WriteBlockInput): WriteBlock | null {
  const { info, status } = input;
  if (info !== undefined && !info.writable) return 'read-only';
  if (!input.connected) return 'disconnected';
  if (status === 'ended') return 'ended';
  if (status !== 'ready') return 'not-ready';
  if (info === undefined) return 'missing';
  if (!input.peerConnected) return 'peer-disconnected';
  if (info.role === 'parameter' && !input.beforeStart) return 'parameter';
  return null;
}

/** Value to write: a boolean for bool, a number for numbers, an item's value (number) or name for enums. */
export type WriteValue = number | boolean | string;

/**
 * Write status (per variable and per run; that of the last write sent).
 * - idle: nothing written.
 * - sending: sent; waiting for the server's answer.
 * - queued: the server accepted it (operation number seq). Waiting to be applied in an ECU step.
 * - applied: applied (atMs: virtual time of the step that applied it).
 * - invalid: the widget refused it before sending (wrong shape or out of range; the text passed to
 *   VariableBinding.reject). Not sent.
 * - dropped: a value held while waiting for the previous write was not sent, because writing became impossible (the
 *   run ended, the ECU disconnected and so on).
 * - refused, failed, no-answer, closed: the kind of the WriteError (the server refused, it was not applied, no answer
 *   came, closed). message is the English text from the server or the library.
 */
export type WriteStatus =
  | { kind: 'idle' }
  | { kind: 'sending' }
  | { kind: 'queued'; seq: number }
  | { kind: 'applied'; atMs: number | null }
  | { kind: 'invalid'; message: string }
  | { kind: 'dropped'; message: string }
  | { kind: 'refused' | 'failed' | 'no-answer' | 'closed'; message: string; code: string | null };

/** Whether the write status is one that went wrong. */
export function isWriteError(
  status: WriteStatus,
): status is Extract<WriteStatus, { message: string }> {
  return 'message' in status;
}

/** Whether the range is set (both ends finite, the lower below the upper). */
export function hasRange(info: Pick<VariableInfo, 'min' | 'max'>): boolean {
  return (
    info.min !== null &&
    info.max !== null &&
    Number.isFinite(info.min) &&
    Number.isFinite(info.max) &&
    info.min < info.max
  );
}
