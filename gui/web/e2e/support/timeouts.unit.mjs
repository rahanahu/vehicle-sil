import assert from 'node:assert/strict';
import test from 'node:test';
import { parseTimeoutScale, readTimeoutScale } from './timeouts.ts';

test('parseTimeoutScale accepts positive finite ASCII decimal numbers', () => {
  for (const [text, expected] of [
    ['3', 3],
    ['0.5', 0.5],
    ['.5', 0.5],
    ['5.', 5],
    ['007', 7],
    ['0.001', 0.001],
    ['2.5', 2.5],
  ]) {
    assert.equal(parseTimeoutScale(text), expected, text);
  }
});

test('parseTimeoutScale rejects non-positive, non-finite, or non-digit inputs', () => {
  const invalid = [
    '',
    '0',
    '0.0',
    '000',
    '000.0',
    '.',
    '-1',
    '+3',
    'abc',
    '3x',
    ' ',
    ' 3',
    '3 ',
    '3\n',
    '1e3',
    '1e-3',
    '0x10',
    '1_000',
    '1.2.3',
    'inf',
    'nan',
    'Infinity',
    'NaN',
    '٢',
    '²',
    '9'.repeat(400),
    `0.${'0'.repeat(400)}1`,
  ];
  for (const text of invalid) {
    assert.equal(parseTimeoutScale(text), undefined, JSON.stringify(text.slice(0, 20)));
  }
});

test('readTimeoutScale defaults to 1 on empty and throws descriptive error on invalid', () => {
  assert.equal(readTimeoutScale(''), 1);
  assert.equal(readTimeoutScale('2.5'), 2.5);
  assert.throws(
    () => readTimeoutScale(' 3'),
    (err) =>
      err instanceof Error &&
      err.message.includes('VEHICLE_SIL_TEST_TIMEOUT_SCALE=" 3" is not a number greater than 0'),
  );
});
