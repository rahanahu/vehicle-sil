// 照合の結果と所見の言葉。実行の画面と設計の画面が使う。知らない値はそのまま出す。
import { CheckOutcome, CheckStatus, Severity } from '@vehicle-sil/gui-client';

function label(table: Record<string, string>, value: string): string {
  return table[value] ?? value;
}

const SEVERITIES: Record<string, string> = {
  error: $localize`誤り`,
  warning: $localize`警告`,
  note: $localize`注記`,
};

/** 重さの名前。知らない重さは誤りとして扱う（ランチャーの決まり）。 */
export function severityLabel(severity: Severity): string {
  return SEVERITIES[severity] ?? SEVERITIES['error'];
}

const CHECK_STATUSES: Record<string, string> = {
  described: $localize`照合した`,
  not_checked: $localize`照合していない`,
  not_described: $localize`申告を読んでいない`,
  failed: $localize`照合できない`,
  skipped: $localize`飛ばした`,
  disabled: $localize`外した`,
};

export const checkStatusLabel = (status: CheckStatus): string => label(CHECK_STATUSES, status);

const CHECK_OUTCOMES: Record<string, string> = {
  passed: $localize`通った`,
  failed: $localize`誤りがある`,
  unreadable: $localize`構成ファイルを読めない`,
  usage: $localize`引数の誤り`,
};

export const checkOutcomeLabel = (result: CheckOutcome): string => label(CHECK_OUTCOMES, result);
