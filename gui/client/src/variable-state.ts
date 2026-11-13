import type { VariableInfo, VariableReading, VariablesStatus } from './variables';

// 変数 1 つの「いまの様子」を決める純粋な関数と、その形。画面の部品（スライダー、ランプなど）は、この形を読めば、
// 値をどう見せるか、書けるか、書き込みがどうなったかを出せる。組み立ては client.variables.bind（variable-binding.ts）。

/**
 * 値の状態（valueCondition が決める）。
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
  /** クライアントと GUI サーバーの接続が開いているか。 */
  connected: boolean;
  /** その ECU が Control Server とつながっているか。 */
  peerConnected: boolean;
  /**
   * 値の読み取りが失敗し続けている（状態は ready のまま、要約の reason に理由がある）。届いている値は最後に読めたもの。
   */
  readFailing?: boolean;
}

/** 値の状態を決める。 */
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

/** 2 つの値の状態が同じか（種類、値、理由）。 */
export function sameCondition(a: ValueCondition, b: ValueCondition): boolean {
  if (a.kind !== b.kind) return false;
  const reading = (c: ValueCondition) => ('reading' in c ? c.reading : undefined);
  const status = (c: ValueCondition) => (c.kind === 'unavailable' ? c.status : undefined);
  const why = (c: ValueCondition) => (c.kind === 'stale' ? c.why : undefined);
  return reading(a) === reading(b) && status(a) === status(b) && why(a) === why(b);
}

/**
 * いま書けない理由（書けるなら null）。
 * - read-only: 読むだけの変数（role が output）。
 * - disconnected: クライアントと GUI サーバーの接続が切れている。
 * - not-ready: 変数の機能が使えない（実行していない、つないでいる、つながらない）。
 * - ended: 実行が終わった。
 * - missing: いまの実行にない変数。
 * - peer-disconnected: その ECU が Control Server とつながっていない。
 * - parameter: parameter は最初のステップの前だけ書ける。
 * - scenario: シナリオの実行中（見るだけ。ランチャーが書き込みを断る）。
 * - no-range: 範囲（min と max）がない変数に、範囲の要る部品（スライダー）を当てた（BindOptions.needsRange）。
 * - no-value: 値がまだないので、値から動かす部品（スライダー）は動かせない（BindOptions.needsValue）。
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
  | 'scenario';

/** writeBlock に渡すもの。 */
export interface WriteBlockInput {
  info: VariableInfo | undefined;
  status: VariablesStatus;
  connected: boolean;
  /** 止めて始めた実行が、最初のステップの前で待っているか（client.time.state の beforeStart）。 */
  beforeStart: boolean;
  /** その ECU が Control Server とつながっているか。 */
  peerConnected: boolean;
}

/**
 * いま書けない理由を決める（書けるなら null）。部品に依らない分だけ: scenario、no-range、no-value は
 * client.variables.bind が足す。
 */
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

/** 書く値: bool は真偽、数は数、列挙は項目の値（数）か名前。 */
export type WriteValue = number | boolean | string;

/**
 * 書き込みの様子（変数ごと、実行ごと。最後に送った書き込みのもの）。
 * - idle: 書いていない。
 * - sending: 送った。サーバーの答えを待っている。
 * - queued: サーバーが受け取った（操作の番号 seq）。ECU のステップで反映されるのを待っている。
 * - applied: 反映された（atMs: 反映したステップの仮想時間）。
 * - invalid: 送る前に部品が断った（形や範囲の誤り。VariableBinding.reject に渡した文）。送っていない。
 * - dropped: 前の書き込みを待つ間に覚えた値を、書けなくなった（実行が終わった、ECU が切れた、など）ので送らなかった。
 * - refused、failed、no-answer、closed: WriteError の kind（サーバーが断った、反映されなかった、答えが来なかった、
 *   閉じた）。message はサーバーかライブラリの英語の文。
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

/** 範囲が決まっている（両端が有限で、下が上より小さい）か。 */
export function hasRange(info: Pick<VariableInfo, 'min' | 'max'>): boolean {
  return (
    info.min !== null &&
    info.max !== null &&
    Number.isFinite(info.min) &&
    Number.isFinite(info.max) &&
    info.min < info.max
  );
}
