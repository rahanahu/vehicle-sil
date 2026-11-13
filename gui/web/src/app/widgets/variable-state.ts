import { VariableInfo, VariableValue } from '@vehicle-sil/gui-client';
import { formatMeasure } from '../core/format/measure-pipe';

// 値を文字にする、入力を数にする（画面の側の決まり）。値の状態（ValueCondition）、書けない理由（WriteBlock）、
// 書き込みの様子（WriteStatus）とそれを決める関数は、クライアントのライブラリ（`@vehicle-sil/gui-client`）にある。

/** 実数を出す小数の桁: 刻み（step）があれば、その桁。なければ 2。 */
export function decimalsOf(info: Pick<VariableInfo, 'step'> | undefined): number {
  const step = info?.step;
  if (step === null || step === undefined || !Number.isFinite(step) || step <= 0) return 2;
  if (step >= 1) return 0;
  return Math.min(6, Math.max(0, Math.ceil(-Math.log10(step) - 1e-9)));
}

/** 値を文字にする（単位は付けない）。bool は onText / offText、列挙は項目の名前（なければ数）。 */
export function formatValue(
  info: VariableInfo | undefined,
  value: VariableValue,
  words: { on: string; off: string; notFinite: string },
): string {
  if (value === null) return words.notFinite;
  if (typeof value === 'boolean') return value ? words.on : words.off;
  if (info?.type === 'enum') {
    return info.enum?.find((item) => item.value === value)?.name ?? String(value);
  }
  if (info?.type === 'integer' || (Number.isInteger(value) && info?.type !== 'real')) {
    return String(value);
  }
  return formatMeasure(value, decimalsOf(info));
}

/**
 * 入力された文字を、変数に書ける数にする。読めなければ理由（kind）を返す: not-a-number（数でない）、
 * not-integer（整数の変数に小数）、below（min より小さい）、above（max より大きい）。
 */
export function parseNumber(
  info: Pick<VariableInfo, 'type' | 'min' | 'max'>,
  text: string,
): { value: number } | { problem: 'not-a-number' | 'not-integer' | 'below' | 'above' } {
  const trimmed = text.trim();
  const value = trimmed === '' ? NaN : Number(trimmed);
  if (!Number.isFinite(value)) return { problem: 'not-a-number' };
  if (info.type === 'integer' && !Number.isSafeInteger(value)) return { problem: 'not-integer' };
  if (info.min !== null && value < info.min) return { problem: 'below' };
  if (info.max !== null && value > info.max) return { problem: 'above' };
  return { value };
}
