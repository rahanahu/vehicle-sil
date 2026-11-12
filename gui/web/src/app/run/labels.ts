// ランチャーの値（英語の固定の名前）を、画面の言葉にする。知らない値はそのまま出す。
import {
  CheckOutcome,
  CheckStatus,
  ControlLink,
  ParticipantState,
  Phase,
  RunOutcome,
  Severity,
  Speed,
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
  failed: $localize`失敗`,
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

/** 結果の名前。読めなかった結果（''）は、そう言う。 */
export const runOutcomeLabel = (result: RunOutcome): string =>
  result === '' ? $localize`結果の名前を読めません` : label(RUN_OUTCOMES, result);

const SIMULATIONS: Record<string, string> = {
  reached_stop_at: $localize`stop_at_ms まで動いた`,
  ended: $localize`participant が止めた`,
  aborted: $localize`打ち切られた`,
  interrupted: $localize`止めた`,
  not_started: $localize`始まらなかった`,
};

export const simulationLabel = (simulation: string): string => label(SIMULATIONS, simulation);

const FAILURE_REASONS: Record<string, string> = {
  left_simulation: $localize`シミュレーションを去った`,
  process_ended: $localize`プロセスが失敗で終わった`,
  ended_before_start: $localize`始まる前に終わった`,
};

/** ランチャーが participant を失敗とした理由（failed の reason）。知らない理由はそのまま出す。 */
export const failureReasonLabel = (reason: string): string => label(FAILURE_REASONS, reason);

const INTERRUPTED_BY: Record<string, string> = {
  'stop command': $localize`停止の命令（画面の「停止」）`,
  'end of input': $localize`標準入力の終わり（GUI サーバーが終わった）`,
  'output closed': $localize`事象を読む側がいなくなった`,
};

/** 何が実行を止めたか（結果の interrupted_by）。シグナルやキーの名前（SIGINT、Ctrl+C）はそのまま出す。 */
export const interruptedByLabel = (by: string): string => label(INTERRUPTED_BY, by);

/** 速さ: 最速、等速、または倍率（ランチャーが書いていなければ —）。 */
export function speedLabel(speed: Speed): string {
  if (speed === null) return '—';
  if (speed === 'max') return $localize`最速`;
  return speed === 1 ? $localize`等速` : $localize`${speed}:factor: 倍`;
}

/**
 * 実時間への遅れの 1 文（色だけに頼らず、文で知らせる）。許容を超えて遅れた刻みがなければ null。いまの遅れ
 * （late_ms）は、許容の内の小さな値が刻みの同期のたびに出たり消えたりするので、文の条件にも中身にも入れない（数の
 * 欄で見せる）。
 */
export function latenessText(late: {
  lateSteps: number;
  maxLateMs: number;
  slippedMs: number;
}): string | null {
  if (late.lateSteps <= 0) return null;
  return $localize`実時間に遅れました: 遅れた刻み ${late.lateSteps}:steps: 回、最大の遅れ ${late.maxLateMs}:max: ms、ずれの合計 ${late.slippedMs}:slipped: ms。`;
}

/** 読み上げる遅れの知らせ（遅れが「ある」になったときだけ変わる。数は入れない）。 */
export const LATE_ANNOUNCEMENT = $localize`実時間に遅れました。`;

/** 送った時刻の命令に、ランチャーがまだ答えていない（command の事象が来ていない）。 */
export const WAITING_TEXT = $localize`送った命令へのランチャーの答えを待っています。`;

/**
 * 変数を読み書きする口のつながり方の 1 文。trouble: この実行では変数を読み書きできない（ランチャーが警告を出す）。
 * 知らない状態は、そのまま出す。
 */
export function controlLabel(link: ControlLink): { text: string; trouble: boolean } {
  switch (link.state) {
    case 'connected':
      return {
        text:
          link.variables === null
            ? $localize`変数: つながった`
            : $localize`変数: つながった（${link.variables}:count: 個）`,
        trouble: false,
      };
    case 'no_hello':
      return { text: $localize`変数: hello が来なかった（読み書きできない）`, trouble: true };
    case 'version_mismatch':
      return {
        text:
          link.version === null
            ? $localize`変数: 形式の版が違う（読み書きできない）`
            : $localize`変数: 形式の版 ${link.version}:version: が違う（読み書きできない）`,
        trouble: true,
      };
    case 'not_offered':
      return {
        text: $localize`変数: あるが、この ECU は読み書きの口を出していない`,
        trouble: false,
      };
    case 'none':
      return { text: $localize`変数: なし`, trouble: false };
    default:
      return { text: $localize`変数: ${link.state}:state:`, trouble: false };
  }
}
