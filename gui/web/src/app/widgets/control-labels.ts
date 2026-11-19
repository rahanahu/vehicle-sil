// 操作できない理由（正面の口の `controls.<op>.reason`）の、画面の言葉。実行の画面と設計の画面が使う。
import { Control, OperationName } from '@vehicle-sil/gui-client';

/** 送った時刻の命令に、ランチャーがまだ答えていない（command の事象が来ていない）。 */
export const WAITING_TEXT = $localize`送った命令へのランチャーの答えを待っています。`;

/**
 * 正面の口が言う、操作できない理由（`controls.<op>.reason`）の、画面の言葉。持ち主（ランチャー、GUI サーバー、
 * 接続）の理由の code から引き、op（どの操作の理由か）で言い分ける。表にない code は、理由の英語の文をそのまま出す。
 */
export function reasonText(op: OperationName, reason: { code: string; message: string }): string {
  return screenText(op, reason) ?? reason.message;
}

/**
 * reasonText と同じ文と、それが英語の文（表にない code で、持ち主の文をそのまま出す）か。英語の文は `lang="en"`
 * を付けて出す。
 */
export function reasonLine(
  op: OperationName,
  reason: { code: string; message: string },
): { text: string; english: boolean } {
  const text = screenText(op, reason);
  return text === null ? { text: reason.message, english: true } : { text, english: false };
}

/**
 * 1 つの操作の説明の文: 押せない理由、押せるなら注意（`caution`）。どちらもなければ null（正面の口の部品と同じ選び方）。
 */
export function controlLine(
  op: OperationName,
  control: Control,
): { text: string; english: boolean } | null {
  const reason = control.reason ?? control.caution;
  return reason ? reasonLine(op, reason) : null;
}

export const isTimeOperation = (op: OperationName): boolean =>
  op !== 'start' && op !== 'stop' && op !== 'force_stop' && op !== 'save';

/** 理由の code の、画面の言葉（表になければ null）。 */
function screenText(op: OperationName, reason: { code: string; message: string }): string | null {
  const time = isTimeOperation(op);
  switch (reason.code) {
    case 'already-running':
      return $localize`実行中は開始できません。`;
    case 'starting':
      return $localize`開始の応答を待っています。`;
    case 'connecting':
      return op === 'start'
        ? $localize`サーバーと揃うまで、開始できません。`
        : $localize`サーバーと揃うまで、操作できません。`;
    case 'offline':
      return $localize`サーバーとの接続が切れています。つなぎ直したら、もう一度押してください。`;
    case 'busy':
      if (op === 'start') return $localize`開始の応答を待っています。`;
      if (time) return WAITING_TEXT;
      return $localize`止めるよう頼みました。`;
    case 'already-stopping':
    case 'already-forced':
      return $localize`止めるよう頼みました。`;
    case 'not-running':
      return time
        ? $localize`時刻の操作は、実行が動いている間だけできます。`
        : $localize`止める実行はありません。`;
    case 'not-paused':
      return op === 'step'
        ? $localize`ステップ送りは、一時停止している間だけできます。`
        : $localize`一時停止していません。`;
    case 'paused':
      return $localize`すでに一時停止しています。`;
    default:
      return null;
  }
}
