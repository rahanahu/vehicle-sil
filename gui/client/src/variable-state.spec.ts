import {
  hasRange,
  isWriteError,
  sameCondition,
  valueCondition,
  writeBlock,
} from './variable-state';
import { VariableInfo } from './variables';

const info = (fields: Partial<VariableInfo>): VariableInfo => ({
  name: 'x',
  type: 'real',
  role: 'input',
  writable: true,
  unit: '',
  min: null,
  max: null,
  step: null,
  description: '',
  enum: null,
  ...fields,
});

const OUTPUT = { role: 'output', writable: false } as const;

describe('the condition of a value and whether it can be written', () => {
  const reading = { value: 1, sampleMs: 100, simMs: 100, paused: false };
  const base = { status: 'ready', known: true, connected: true, peerConnected: true } as const;

  it('tells a value that is missing, waited for, live, of a stopped time, old or the last of a run', () => {
    expect(valueCondition({ ...base, status: 'off', known: false, reading: undefined }).kind).toBe(
      'unavailable',
    );
    expect(valueCondition({ ...base, known: false, reading: undefined }).kind).toBe('missing');
    expect(valueCondition({ ...base, reading: undefined }).kind).toBe('waiting');
    expect(valueCondition({ ...base, reading }).kind).toBe('live');
    expect(valueCondition({ ...base, reading: { ...reading, paused: true } }).kind).toBe('paused');
    expect(valueCondition({ ...base, reading, connected: false })).toMatchObject({
      kind: 'stale',
      why: 'connection',
    });
    expect(valueCondition({ ...base, reading, status: 'connecting' })).toMatchObject({
      kind: 'stale',
      why: 'reconnecting',
    });
    expect(valueCondition({ ...base, reading, peerConnected: false })).toMatchObject({
      kind: 'stale',
      why: 'peer',
    });
    // 値の読み取りが失敗し続けている（ready のまま理由がある）: 最後に読めた値は古い
    expect(valueCondition({ ...base, reading, readFailing: true })).toMatchObject({
      kind: 'stale',
      why: 'reading',
    });
    expect(valueCondition({ ...base, reading, status: 'ended', readFailing: true }).kind).toBe(
      'ended',
    );
    expect(valueCondition({ ...base, reading, status: 'ended', peerConnected: false }).kind).toBe(
      'ended',
    );
  });

  it('lets a parameter be written only before the first step', () => {
    const parameter = info({ role: 'parameter' });
    const ready = { status: 'ready', connected: true, peerConnected: true } as const;
    expect(writeBlock({ ...ready, info: parameter, beforeStart: true })).toBeNull();
    expect(writeBlock({ ...ready, info: parameter, beforeStart: false })).toBe('parameter');
    expect(writeBlock({ ...ready, info: info({}), beforeStart: false })).toBeNull();
    expect(writeBlock({ ...ready, info: info(OUTPUT), beforeStart: true })).toBe('read-only');
    expect(writeBlock({ ...ready, info: undefined, beforeStart: true })).toBe('missing');
    expect(writeBlock({ ...ready, peerConnected: false, info: info({}), beforeStart: true })).toBe(
      'peer-disconnected',
    );
    expect(writeBlock({ ...ready, connected: false, info: info({}), beforeStart: true })).toBe(
      'disconnected',
    );
    expect(writeBlock({ ...ready, status: 'ended', info: info({}), beforeStart: false })).toBe(
      'ended',
    );
    expect(writeBlock({ ...ready, status: 'failed', info: info({}), beforeStart: false })).toBe(
      'not-ready',
    );
  });

  it('compares two conditions by their kind, value and reason', () => {
    const other = { ...reading };
    expect(sameCondition({ kind: 'live', reading }, { kind: 'live', reading })).toBe(true);
    expect(sameCondition({ kind: 'live', reading }, { kind: 'live', reading: other })).toBe(false);
    expect(sameCondition({ kind: 'live', reading }, { kind: 'paused', reading })).toBe(false);
    expect(
      sameCondition(
        { kind: 'stale', reading, why: 'peer' },
        { kind: 'stale', reading, why: 'connection' },
      ),
    ).toBe(false);
    expect(
      sameCondition({ kind: 'unavailable', status: 'off' }, { kind: 'unavailable', status: 'off' }),
    ).toBe(true);
    expect(
      sameCondition(
        { kind: 'unavailable', status: 'off' },
        { kind: 'unavailable', status: 'failed' },
      ),
    ).toBe(false);
    expect(sameCondition({ kind: 'waiting' }, { kind: 'waiting' })).toBe(true);
  });

  it('tells a failed write by its sentence, and a range by both of its ends', () => {
    expect(isWriteError({ kind: 'applied', atMs: 1 })).toBe(false);
    expect(isWriteError({ kind: 'invalid', message: 'x' })).toBe(true);
    expect(isWriteError({ kind: 'no-answer', message: 'x', code: null })).toBe(true);
    expect(hasRange({ min: 0, max: 100 })).toBe(true);
    expect(hasRange({ min: null, max: 100 })).toBe(false);
    expect(hasRange({ min: 5, max: 5 })).toBe(false);
  });
});
