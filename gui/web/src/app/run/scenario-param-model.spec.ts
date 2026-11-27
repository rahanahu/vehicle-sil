import { CheckResult, CheckScenario } from '@vehicle-sil/gui-client';
import {
  buildParamRows,
  caseOptionsFor,
  checkSelectionGuidance,
  coerceSelectParamValue,
  isParamRecord,
  normalizeProvenance,
  parseParamText,
} from './scenario-param-model';

function checkResult(overrides: Partial<CheckResult> = {}): CheckResult {
  return {
    file: 'simulation.yaml',
    result: 'passed',
    errors: 0,
    warnings: 0,
    notes: 0,
    stop_at_ms: null,
    participants: [],
    networks: [],
    findings: [],
    scenario: null,
    ...overrides,
  };
}

describe('scenario-param-model', () => {
  describe('checkSelectionGuidance', () => {
    it('identifies missing variant and missing case usage checks', () => {
      expect(
        checkSelectionGuidance(
          checkResult({ result: 'usage', variant: null, variants: ['small', 'big'] }),
        ),
      ).toBe('variant');
      expect(
        checkSelectionGuidance(
          checkResult({
            result: 'usage',
            variant: 'small',
            variants: ['small', 'big'],
            scenario: { file: 'scenarios/a.yaml', name: null, case: null, cases: ['c1'], steps: 0 },
          }),
        ),
      ).toBe('case');
      expect(
        checkSelectionGuidance(
          checkResult({ result: 'failed', variant: null, variants: ['small', 'big'] }),
        ),
      ).toBeNull();
      expect(checkSelectionGuidance(checkResult({ result: 'usage' }))).toBeNull();
    });
  });

  describe('caseOptionsFor', () => {
    it('disables cases whose only restriction excludes the selected variant', () => {
      const scenario: CheckScenario = {
        file: 'scenarios/cases.yaml',
        name: null,
        steps: 0,
        cases: ['all', 'big_only', 'small_or_quiet'],
        case_details: [
          { name: 'all', only: null },
          { name: 'big_only', only: ['big'] },
          { name: 'small_or_quiet', only: ['small', 'quiet'] },
        ],
      };
      expect(caseOptionsFor(scenario, 'small')).toEqual([
        { name: 'all', only: null, disabled: false },
        { name: 'big_only', only: ['big'], disabled: true },
        { name: 'small_or_quiet', only: ['small', 'quiet'], disabled: false },
      ]);
      expect(caseOptionsFor(scenario, 'big')[1]?.disabled).toBe(false);
      expect(caseOptionsFor(scenario, 'big')[2]?.disabled).toBe(true);
    });
  });

  describe('parseParamText & coerceSelectParamValue', () => {
    it('keeps string-typed inputs as strings without numeric or boolean coercion', () => {
      expect(parseParamText('0123', 'string', 'string', '0000')).toEqual({
        ok: true,
        value: '0123',
      });
      expect(parseParamText('50', 'string', 'string', '10')).toEqual({ ok: true, value: '50' });
      expect(parseParamText('true', 'string', 'string', 'false')).toEqual({
        ok: true,
        value: 'true',
      });
      expect(coerceSelectParamValue('50', 'string', '50')).toBe('50');
    });

    it('validates integer, number, and boolean inputs strictly', () => {
      expect(parseParamText('42', 'integer', 'integer', 10)).toEqual({ ok: true, value: 42 });
      expect(parseParamText('0x1F', 'integer', 'integer', 10)).toEqual({ ok: true, value: 31 });
      expect(parseParamText('3.5', 'integer', 'integer', 10)).toEqual({ ok: false });
      expect(parseParamText('', 'integer', 'integer', 10)).toEqual({ ok: false });

      expect(parseParamText('3.5', 'number', 'number', 1)).toEqual({ ok: true, value: 3.5 });
      expect(parseParamText('abc', 'number', 'number', 1)).toEqual({ ok: false });

      expect(parseParamText('true', 'boolean', 'boolean', false)).toEqual({
        ok: true,
        value: true,
      });
      expect(parseParamText('false', 'boolean', 'boolean', true)).toEqual({
        ok: true,
        value: false,
      });
      expect(parseParamText('yes', 'boolean', 'boolean', true)).toEqual({ ok: false });

      expect(coerceSelectParamValue('100', 'integer', 50)).toBe(100);
      expect(coerceSelectParamValue('true', 'boolean', false)).toBe(true);
    });

    it('handles untyped missing parameters without corrupting leading-zero codes', () => {
      expect(parseParamText('50', 'string', null, undefined)).toEqual({ ok: true, value: 50 });
      expect(parseParamText('0123', 'string', null, undefined)).toEqual({
        ok: true,
        value: '0123',
      });
      expect(parseParamText('true', 'string', null, undefined)).toEqual({ ok: true, value: true });
    });
  });

  describe('buildParamRows', () => {
    it('merges params, param_meta, missing_params, overrides, and drafts with missing first', () => {
      const scenario: CheckScenario = {
        file: 'scenarios/t.yaml',
        name: null,
        steps: 2,
        params: [
          { name: 'throttle', value: 50, from: 'case', type: 'integer' },
          {
            name: 'mode',
            value: 'eco',
            from: 'scenario',
            type: 'string',
            values: ['eco', 'sport'],
          },
          { name: 'flag', value: true, from: 'variant', type: 'boolean' },
        ],
        param_meta: [
          { name: 'req_code', type: 'string', description: 'required tag' },
          { name: 'throttle', type: 'integer' },
        ],
        missing_params: ['req_code'],
      };
      const rows = buildParamRows(scenario, { throttle: 80 }, { throttle: 'bad' });
      expect(rows.map((r) => r.name)).toEqual(['req_code', 'throttle', 'mode', 'flag']);
      expect(rows[0]).toMatchObject({
        missing: true,
        provenance: 'missing',
        control: 'string',
        description: 'required tag',
      });
      expect(rows[1]).toMatchObject({
        overridden: true,
        provenance: 'override',
        control: 'integer',
        displayText: 'bad',
        invalid: true,
      });
      expect(rows[2]).toMatchObject({
        provenance: 'scenario',
        control: 'select',
        values: ['eco', 'sport'],
      });
      expect(rows[3]).toMatchObject({ provenance: 'variant', control: 'boolean' });
    });
  });

  describe('normalizeProvenance & isParamRecord', () => {
    it('normalizes launcher provenance strings and validates parameter records', () => {
      expect(normalizeProvenance('request_file', false, false)).toBe('override');
      expect(normalizeProvenance('command_line', false, false)).toBe('override');
      expect(normalizeProvenance('configuration', false, false)).toBe('configuration');
      expect(normalizeProvenance('template_default', false, false)).toBe('default');
      expect(isParamRecord({ a: 1, b: '0123', c: true })).toBe(true);
      expect(isParamRecord({ a: Number.NaN })).toBe(false);
      expect(isParamRecord({ a: null })).toBe(false);
      expect(isParamRecord([])).toBe(false);
    });
  });
});
