import {
  VariableInfo,
  VariableReading,
  VariableValue,
  VariablesStatus,
} from '@vehicle-sil/gui-client';
import { formatMeasure } from '../core/format/measure-pipe';

/**
 * 部品が出す値の状態（純粋な関数 valueCondition が決める）。
 * - unavailable: 変数の機能が使えない（実行していない、この実行に変数はない、つないでいる、つながらない）。
 * - missing: いまの実行にない変数。
 * - waiting: いまの実行にあるが、値がまだ届いていない。
 * - live: 動いている途中の最後の値。
 * - paused: 止まっている時刻の値（その時刻より前に始まったステップの値は入っている）。
 * - stale: 古いかもしれない値（why: 画面とサーバーの接続が切れた、ランチャーにつなぎ直している、ECU が切れた、
 *   値の読み取りが失敗し続けている）。
 * - ended: 終わった実行の、最後に読めた値。
 */
export type ValueCondition =
  | { kind: 'unavailable'; status: VariablesStatus }
  | { kind: 'missing' }
  | { kind: 'waiting' }
  | { kind: 'live'; reading: VariableReading }
  | { kind: 'paused'; reading: VariableReading }
  | {
      kind: 'stale';
      reading: VariableReading;
      why: 'connection' | 'reconnecting' | 'peer' | 'reading';
    }
  | { kind: 'ended'; reading: VariableReading };

/** valueCondition に渡すもの。 */
export interface ConditionInput {
  /** 変数の機能の状態（client.variables.state の status）。 */
  status: VariablesStatus;
  /** いまの実行の一覧にある変数か。 */
  known: boolean;
  /** 最後の値（なければ undefined）。 */
  reading: VariableReading | undefined;
  /** 画面と GUI サーバーの接続が開いているか。 */
  connected: boolean;
  /** その ECU が Control Server とつながっているか。 */
  peerConnected: boolean;
  /**
   * 値の読み取りが失敗し続けている（状態は ready のまま、要約の reason に理由がある）。届いている値は最後に読めたもの。
   */
  readFailing?: boolean;
}

/** 部品が出す値の状態を決める。 */
export function valueCondition(input: ConditionInput): ValueCondition {
  const { status, known, reading } = input;
  // 一覧がある状態（ready、ended、つなぎ直している間の connecting）でなければ、変数の名前も確かでない
  if (!known) {
    return status === 'ready' || status === 'ended'
      ? { kind: 'missing' }
      : { kind: 'unavailable', status };
  }
  if (reading === undefined) {
    return status === 'ready' || status === 'ended'
      ? { kind: 'waiting' }
      : { kind: 'unavailable', status };
  }
  if (!input.connected) return { kind: 'stale', reading, why: 'connection' };
  if (status === 'ended') return { kind: 'ended', reading };
  if (status !== 'ready') return { kind: 'stale', reading, why: 'reconnecting' };
  if (!input.peerConnected) return { kind: 'stale', reading, why: 'peer' };
  if (input.readFailing) return { kind: 'stale', reading, why: 'reading' };
  return reading.paused ? { kind: 'paused', reading } : { kind: 'live', reading };
}

/**
 * いま書けない理由（書けるなら null）。
 * - read-only: 読むだけの変数（role が output）。
 * - disconnected: 画面と GUI サーバーの接続が切れている。
 * - not-ready: 変数の機能が使えない（実行していない、つないでいる、つながらない）。
 * - ended: 実行が終わった。
 * - missing: いまの実行にない変数。
 * - peer-disconnected: その ECU が Control Server とつながっていない。
 * - parameter: parameter は最初のステップの前だけ書ける。
 * - no-range: 範囲（min と max）がない変数に、範囲の要る部品（スライダー）を当てた。
 * - no-value: 値がまだないので、値から動かす部品（スライダー）は動かせない。
 */
export type WriteBlock =
  | 'read-only'
  | 'disconnected'
  | 'not-ready'
  | 'ended'
  | 'missing'
  | 'peer-disconnected'
  | 'parameter'
  | 'no-range'
  | 'no-value'
  /** シナリオの実行中（ランチャーが書き込みを断る。部品の bindVariable が足す）。 */
  | 'scenario';

/** writeBlock に渡すもの。 */
export interface WriteBlockInput {
  info: VariableInfo | undefined;
  status: VariablesStatus;
  connected: boolean;
  /** 止めて始めた実行が、最初のステップの前で待っているか（SimulationTime.beforeStart）。 */
  beforeStart: boolean;
  /** その ECU が Control Server とつながっているか。 */
  peerConnected: boolean;
}

/** いま書けない理由を決める（書けるなら null）。 */
export function writeBlock(input: WriteBlockInput): WriteBlock | null {
  const { info, status } = input;
  if (info !== undefined && !info.writable) return 'read-only';
  if (!input.connected) return 'disconnected';
  if (status === 'ended') return 'ended';
  if (status !== 'ready') return 'not-ready';
  if (info === undefined) return 'missing';
  if (!input.peerConnected) return 'peer-disconnected';
  if (info.role === 'parameter' && !input.beforeStart) return 'parameter';
  return null;
}

/**
 * 書き込みの様子（部品ごと。最後に送った書き込みのもの）。
 * - idle: 書いていない。
 * - sending: 送った。サーバーの答えを待っている。
 * - queued: サーバーが受け取った（操作の番号 seq）。ECU のステップで反映されるのを待っている。
 * - applied: 反映された（atMs: 反映したステップの仮想時間）。
 * - invalid: 送る前に部品が断った（形や範囲の誤り）。送っていない。
 * - dropped: 前の書き込みを待つ間にためた値を、書けなくなった（実行が終わった、ECU が切れた、など）ので送らなかった。
 * - refused、failed、no-answer、closed: WriteError の kind（サーバーが断った、反映されなかった、答えが来なかった、
 *   閉じた）。message はサーバーかライブラリの英語の文（invalid は画面の言葉）。
 */
export type WriteStatus =
  | { kind: 'idle' }
  | { kind: 'sending' }
  | { kind: 'queued'; seq: number }
  | { kind: 'applied'; atMs: number | null }
  | { kind: 'invalid'; message: string }
  | { kind: 'dropped'; message: string }
  | { kind: 'refused' | 'failed' | 'no-answer' | 'closed'; message: string; code: string | null };

/** 書き込みの様子が、うまくいかなかったものか。 */
export function isWriteError(
  status: WriteStatus,
): status is Extract<WriteStatus, { message: string }> {
  return 'message' in status;
}

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
