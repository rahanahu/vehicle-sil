import { Pipe, PipeTransform } from '@angular/core';

/**
 * 計測値の表示の書式。桁区切りを入れず、小数の桁を固定し、小数点は「.」。
 * locale の数値の書式（1,533.3 など）には任せない（言語を切り替えても同じ表示にするため）。
 * 値がないときは「—」、有限でない値は NaN、Infinity、-Infinity のまま出す。
 */
export function formatMeasure(value: number | null | undefined, digits = 2): string {
  if (value === null || value === undefined) {
    return '—';
  }
  if (!Number.isFinite(value)) {
    return String(value);
  }
  const text = value.toFixed(digits);
  // -0.00 のような負のゼロを 0.00 にそろえる
  return Number(text) === 0 ? text.replace('-', '') : text;
}

@Pipe({ name: 'measure' })
export class MeasurePipe implements PipeTransform {
  transform(value: number | null | undefined, digits = 2): string {
    return formatMeasure(value, digits);
  }
}
