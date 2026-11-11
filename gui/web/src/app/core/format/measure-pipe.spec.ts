import { formatMeasure } from './measure-pipe';

describe('formatMeasure', () => {
  it('uses fixed decimals without grouping', () => {
    expect(formatMeasure(1533.25, 1)).toBe('1533.3');
    expect(formatMeasure(1234567.891)).toBe('1234567.89');
    expect(formatMeasure(12, 0)).toBe('12');
    expect(formatMeasure(0.5, 3)).toBe('0.500');
  });

  it('keeps the sign, but not on zero', () => {
    expect(formatMeasure(-2.345)).toBe('-2.35');
    expect(formatMeasure(-0.001)).toBe('0.00');
  });

  it('shows missing and non-finite values', () => {
    expect(formatMeasure(null)).toBe('—');
    expect(formatMeasure(undefined)).toBe('—');
    expect(formatMeasure(Number.NaN)).toBe('NaN');
    expect(formatMeasure(Number.NEGATIVE_INFINITY)).toBe('-Infinity');
  });
});
