// ランチャーの値（英語の固定の名前）を、画面の言葉にする。知らない値はそのまま出す。
import {
  CheckOutcome,
  CheckStatus,
  ParticipantState,
  Phase,
  RunOutcome,
  Severity,
} from '../core/api';

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

const PHASES: Record<string, string> = {
  checking: $localize`照合`,
  starting: $localize`起動`,
  waiting: $localize`参加を待つ`,
  running: $localize`実行`,
  stopping: $localize`停止中`,
  ended: $localize`終了`,
};

export const phaseLabel = (phase: Phase): string => label(PHASES, phase);

const CHECK_STATUSES: Record<string, string> = {
  described: $localize`照合した`,
  not_checked: $localize`照合していない`,
  failed: $localize`照合できない`,
  skipped: $localize`飛ばした`,
  disabled: $localize`外した`,
};

export const checkStatusLabel = (status: CheckStatus): string => label(CHECK_STATUSES, status);

const PARTICIPANT_STATES: Record<string, string> = {
  disabled: $localize`外した`,
  started: $localize`起動した`,
  joined: $localize`参加した`,
  ended: $localize`終了した`,
  stop_requested: $localize`停止を求めた`,
  killed: $localize`強制終了した`,
  not_started: $localize`起動していない`,
};

export function participantStateLabel(state: ParticipantState | null): string {
  return state === null ? $localize`待っている` : label(PARTICIPANT_STATES, state);
}

const CHECK_OUTCOMES: Record<string, string> = {
  passed: $localize`通った`,
  failed: $localize`誤りがある`,
  unreadable: $localize`構成ファイルを読めない`,
  usage: $localize`引数の誤り`,
};

export const checkOutcomeLabel = (result: CheckOutcome): string => label(CHECK_OUTCOMES, result);

const RUN_OUTCOMES: Record<string, string> = {
  passed: $localize`成功`,
  failed: $localize`失敗`,
  interrupted: $localize`中断`,
  check_failed: $localize`照合の誤りで起動しなかった`,
  unreadable: $localize`構成ファイルを読めない`,
  setup_failed: $localize`準備に失敗した`,
  usage: $localize`引数の誤り`,
};

export const runOutcomeLabel = (result: RunOutcome): string => label(RUN_OUTCOMES, result);

const SIMULATIONS: Record<string, string> = {
  reached_stop_at: $localize`stop_at_ms まで動いた`,
  ended: $localize`participant が止めた`,
  aborted: $localize`中断した`,
  interrupted: $localize`止めた`,
  not_started: $localize`始まらなかった`,
};

export const simulationLabel = (simulation: string): string => label(SIMULATIONS, simulation);
