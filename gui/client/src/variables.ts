import { HttpError, HttpTransport, errorMessage } from './http';
import { Open } from './protocol';
import { ErrorSink, Readable, Writable, rethrowLater } from './values';
import type { BindOptions, VariableBinding } from './variable-binding';

/** 変数の値: 実数と整数と列挙は数、bool は真偽。有限でない実数（NaN、無限大）は null。 */
export type VariableValue = number | boolean | null;

/** 変数の名指し: どの ECU（peer）の、どの変数か。 */
export interface VariableRef {
  peer: string;
  name: string;
}

/** 変数の定義（ECU の宣言から）。 */
export interface VariableInfo {
  name: string;
  type: Open<'bool' | 'integer' | 'real' | 'enum'>;
  /** input: 実行中も書ける、parameter: 最初のステップの前だけ書ける、output: 読むだけ。 */
  role: Open<'input' | 'output' | 'parameter'>;
  /** 書けるか（role が input か parameter）。 */
  writable: boolean;
  unit: string;
  min: number | null;
  max: number | null;
  /** 編集の刻みの目安（強制ではない）。 */
  step: number | null;
  description: string;
  /** 列挙の項目（type が enum のとき。ほかは null）。 */
  enum: { name: string; value: number }[] | null;
}

/** 変数を持つ participant。 */
export interface VariablePeer {
  name: string;
  /** いま Control Server とつながっているか。 */
  connected: boolean;
  variables: VariableInfo[];
}

/**
 * 変数の機能の状態。unknown: まだサーバーから聞いていない、off: この実行に変数はない（reason に理由）、
 * connecting: ランチャーの Control Server につなぎに行っている（切れてつなぎ直している間も。reason に理由）、
 * ready: 読み書きできる、failed: つながらなかった（reason に理由）、ended: 実行が終わった（値は最後のもの）。
 */
export type VariablesStatus = Open<'unknown' | 'off' | 'connecting' | 'ready' | 'failed' | 'ended'>;

/** 変数の一覧と状態（いまの実行のもの）。 */
export interface VariablesState {
  /** どの実行のものか（実行がなければ null）。 */
  session: number | null;
  status: VariablesStatus;
  /** 使えない、つながらない理由（サーバーの英語の文）。なければ null。 */
  reason: string | null;
  peers: VariablePeer[];
  /** 上限を超えたのでサーバーが覚えていない participant と変数の数。 */
  peersOmitted: number;
  variablesOmitted: number;
}

/** 読んだ値 1 つと、それがいつのものか。 */
export interface VariableReading {
  value: VariableValue;
  /** この値を読んだステップの仮想時間（ミリ秒）。 */
  sampleMs: number;
  /** この値が届いたときの controller の仮想時間（ミリ秒。わからなければ null）。 */
  simMs: number | null;
  /**
   * 止まっている時刻の値か。true なら、simMs より前に始まったステップの値は、この値に入っている（止まっている
   * 時刻の値として見せてよい）。false なら、動いている途中の最後の値。
   */
  paused: boolean;
}

/** watch している変数の、最後の値。`values[peer]?.[name]`。値がまだ届いていない変数は、ない。 */
export type WatchValues = Readonly<Record<string, Readonly<Record<string, VariableReading>>>>;

/** 履歴の 1 点。 */
export interface HistoryPoint {
  sampleMs: number;
  value: VariableValue;
}

/** watch の設定。 */
export interface WatchOptions {
  /**
   * 値を受け取る間隔（ミリ秒。既定 100）。サーバーの下限（limits の watch_period_ms_min、ふつう 20）より短くは
   * ならない。同じ変数をほかの watch がもっと短い間隔で見ていれば、その間隔で届く。
   */
  periodMs?: number;
  /**
   * 変数ごとに覚えておく履歴の点の数（既定 0: 覚えない。上限 HISTORY_MAX。数でない値と負は 0）。古いものから
   * 捨てる。場所は点が届いてから取る。全部の変数の履歴を合わせて HISTORY_TOTAL_MAX 点までで、届いたら、どの履歴も
   * そのときの大きさのまま古い点を捨てる（頼んだ数より少なくなる）。実行が替わると、履歴は空から始まる。
   */
  history?: number;
}

/** 変数の watch。使い終わったら close() する（しないと、値が届き続ける）。 */
export interface VariableWatch {
  /** watch している変数の最後の値。値が届くたびに新しいオブジェクトになる。 */
  readonly values: Readable<WatchValues>;
  /** 変数の最後の値（まだなければ undefined）。 */
  value(ref: VariableRef): VariableReading | undefined;
  /** 変数の履歴（古い順。WatchOptions.history の数まで）。呼ぶたびに新しい配列。 */
  history(ref: VariableRef): HistoryPoint[];
  /** やめる。同じ変数を見ているほかの watch がなければ、サーバーの購読もやめる。 */
  close(): void;
}

/** サーバーが watch を断ったこと。 */
export interface WatchProblem {
  /** `invalid-watch` か `too-many-watches`。 */
  code: string;
  /** サーバーの英語の文。 */
  message: string;
}

/** 書き込みの結果（操作）の、participant ごとの状態。 */
export interface OperationPeer {
  /** sent、queued、disconnected（途中）。applied、late、rejected、cancelled、invalid、busy、unknown（確定）。 */
  state: string;
  /** 反映したステップの仮想時間（ミリ秒）。 */
  appliedAtMs: number | null;
  /** 指定の時刻より後のステップで反映した。 */
  late: boolean;
  /** うまくいかなかった理由（ECU か Control Server の英語の文）。 */
  reason: string | null;
}

/** 書き込みの結果（操作）。 */
export interface OperationView {
  session: number;
  /** 操作の番号（`api/write` の答えの operation）。 */
  seq: number;
  /**
   * 全体の状態。sent、queued（途中）。確定したら、全員が同じならその状態（applied、late、rejected、cancelled、
   * invalid、busy、unknown）、participant によって違えば mixed。
   */
  state: string;
  /** 確定したか（もう変わらない）。 */
  done: boolean;
  peers: Record<string, OperationPeer>;
}

/** 書き込みの設定。 */
export interface WriteOptions {
  /** 反映する仮想時刻（ミリ秒）。省くと次のステップ。 */
  atMs?: number;
  /** 指定の時刻に間に合わなかったとき: reject（既定。反映しない）、apply（次のステップで反映する）。 */
  late?: 'reject' | 'apply';
  /**
   * 結果（反映されたか）を待つ時間（ミリ秒。既定 WRITE_TIMEOUT_MS）。止まっている間や、先の時刻（atMs）への
   * 書き込みは、シミュレーションがそこまで進むまで確定しないので、長くするか、結果を operations で見る。
   * `Infinity` は時間で切らない（実行が替わるか close() するまで待つ）。約 24.8 日（2^31-1 ms）より長い値はそこに
   * 丸める。数でない値と 0 以下は既定の時間。HTTP の答えが来てから数える。
   */
  timeoutMs?: number;
  /**
   * サーバーが書き込みを受け取って操作の番号を返したときに、1 回呼ぶ（結果より前。断られたとき、答えがなかった
   * ときは呼ばない）。番号で `operations` の途中の状態（queued など）を追える。部品が「受け付けた、反映を待って
   * いる」を見せたり、次の書き込みを送ってよいと知ったりするため。
   */
  onAccepted?: (operation: { session: number; seq: number }) => void;
}

/**
 * 書き込みがうまくいかなかったこと。
 * - refused: サーバーが受け付けなかった（型や範囲の誤り、知らない変数、実行がない、など。code に誤りの名前）。
 *   送られていないので、直して送り直してよい。
 * - failed: 受け付けたが、反映されなかった（operation に、participant ごとの状態と理由）。
 * - no-answer: 反映されたかどうかわからない。待つ時間の間に結果が来なかった（operation に、わかっている最後の
 *   状態）、HTTP の答えがなかった、サーバーが途中で失敗した（500）、Control Server の答えを待ちきれなかった（code は `write-timeout`）か
 *   途中で切れた（`write-failed`）、答えの前に実行が替わった、つなぎ直している間に確定して結果が失われた（要約の
 *   一覧になく、境目 operations_from_seq より古い）、サーバーが未確定の操作を覚えきれずに追うのをやめた。
 *   **そのまま送り直すと、二重の書き込みになることがある**: 先に operations と値を見る。
 * - closed: クライアントを閉じた（反映されたかどうかはわからない）。
 */
export class WriteError extends Error {
  constructor(
    readonly kind: 'refused' | 'failed' | 'no-answer' | 'closed',
    message: string,
    /**
     * サーバーの誤りの名前（refused のとき `unknown-variable`、`not-writable`、`invalid-field` など。no-answer の
     * とき `write-timeout`、`write-failed`。HTTP の誤りでなければ null）。
     */
    readonly code: string | null = null,
    /** 操作（番号がわかった後）。 */
    readonly operation: OperationView | null = null,
  ) {
    super(message);
    this.name = 'WriteError';
  }
}

/** 変数の、読むための口と書き込み（GuiClient.variables）。 */
export interface ClientVariables {
  /** 変数の一覧と状態。 */
  readonly state: Readable<VariablesState>;
  /** 最近の操作（書き込みの結果。古い順、OPERATIONS_KEPT 件まで。ほかのクライアントの書き込みも入る）。 */
  readonly operations: Readable<readonly OperationView[]>;
  /**
   * サーバーが watch を断ったこと（`invalid-watch`、`too-many-watches`。なければ null）。断られた購読の値は
   * 届かない。watch を作り直すか減らして、購読を送り直すと null に戻る。ライブラリが送る購読は、サーバーの上限に
   * 収めてあるので、ふつうは起きない（上限の違うサーバーのとき）。
   */
  readonly watchProblem: Readable<WatchProblem | null>;
  /**
   * 変数の値を受け取り始める。`'all'` は全部の変数。同じ変数を見る watch がいくつあっても、サーバーへの購読は
   * 変数ごとに 1 つにまとめる。つなぎ直したら、自動で購読し直す。実行が替わっても続く（新しい実行の値が届く）。
   */
  watch(variables: readonly VariableRef[] | 'all', options?: WatchOptions): VariableWatch;
  /**
   * 変数を書く: `write({Engine: {'engine.throttle': 50}})`。いまの実行（state の session）に向けて送り、サーバーは
   * その実行が動いていなければ断る（refused、code session-changed / not-running。実行がまだわからなければ、送らずに
   * not-running）。列挙は値（数）か項目の名前。反映されたら（applied）
   * その操作で解ける。ほかは WriteError で断る（kind で分ける）。同じ書き込みを自動で送り直すことはない。
   */
  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<OperationView>;
  /**
   * 変数 1 つと、画面の部品 1 つをつなぐ: 値の状態（古い、まだない、止まっている時刻の値、など）、書けるかどうかと
   * 理由、書き込みの様子を 1 つの読める値（state）で出し、write(value) で書く。スライダー、スイッチ、ランプのような
   * 部品は、これだけで作れる。同じ変数を何か所で bind しても、書き込みは変数ごと・実行ごとに 1 つにまとまる
   * （重ねて送らない、どこからも同じ様子が見える、bind を作り直しても様子は残る）。使い終わったら close() する。
   */
  bind(ref: VariableRef, options?: BindOptions): VariableBinding;
}

/** 書き込みの結果を待つ時間の既定（ミリ秒）。 */
export const WRITE_TIMEOUT_MS = 10_000;
/** 履歴の点の数の上限（変数ごと）。 */
export const HISTORY_MAX = 100_000;
/**
 * 履歴のために取る場所の、全部の変数を合わせた上限（点の数。1 点は 17 バイト）。届いたら、どの変数の履歴も
 * それ以上は伸びず、そのときの大きさのまま古い点を捨てる（頼んだ数より少ない点しか残らない）。
 */
export const HISTORY_TOTAL_MAX = 2_000_000;
/** 履歴が最初に取る場所（点の数）。 */
const HISTORY_FIRST = 16;
/** watch の 1 通の大きさの上限（バイト。サーバーは 256 KiB を超える 1 通で接続を閉じる）。超えるなら「全部」を頼む。 */
const WATCH_MESSAGE_BYTES = 200 * 1024;
/** setTimeout が受け取れる最長（ミリ秒）。これを超える値を渡すと、すぐに発火する。 */
const TIMER_MAX_MS = 2 ** 31 - 1;
/** 覚えておく最近の操作の数。 */
export const OPERATIONS_KEPT = 64;
/** watch の間隔の既定（ミリ秒）。 */
export const WATCH_PERIOD_MS = 100;
/**
 * サーバーへの購読の間隔（ミリ秒）。頼まれた間隔を、これ以下で最も長いものに丸める（頼まれたより遅くはしない）。
 * 間隔ごとに 1 つの購読にまとめるので、購読の数はこの数まで（サーバーの上限は接続ごとに 16）。
 */
export const WATCH_PERIODS_MS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10_000, 60_000] as const;
/** 1 つの購読に名前で挙げる変数の数の上限の既定（超えたら、その購読は「全部」を頼む）。 */
const WATCH_VARIABLES = 2000;

const NO_VARIABLES: VariablesState = {
  session: null,
  status: 'unknown',
  reason: null,
  peers: [],
  peersOmitted: 0,
  variablesOmitted: 0,
};

/** UTF-8 にしたときのバイト数。 */
function utf8Length(textValue: string): number {
  let bytes = 0;
  for (let i = 0; i < textValue.length; i++) {
    const code = textValue.charCodeAt(i);
    bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : 3; // 対の片方は 3 と数える（多めに見積もる）
  }
  return bytes;
}

/** 変数の名指しを 1 つの文字列にする（participant 名にも変数名にも NUL は入らない）。 */
const keyOf = (peer: string, name: string) => `${peer}\u0000${name}`;

type Fields = Record<string, unknown>;
const isObject = (value: unknown): value is Fields =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const text = (value: unknown, fallback = '') => (typeof value === 'string' ? value : fallback);
const numberOrNull = (value: unknown) =>
  typeof value === 'number' && Number.isFinite(value) ? value : null;
const count = (value: unknown) => (typeof value === 'number' && value >= 0 ? value : 0);

/** 要約の `variables` を読む。読めない部分は、ないものとして読み飛ばす（足すのは互換）。 */
export function readVariables(value: unknown, session: number | null): VariablesState {
  if (!isObject(value)) return { ...NO_VARIABLES, session };
  const peers: VariablePeer[] = [];
  for (const p of Array.isArray(value['peers']) ? value['peers'] : []) {
    if (!isObject(p) || typeof p['name'] !== 'string') continue;
    const variables: VariableInfo[] = [];
    for (const v of Array.isArray(p['variables']) ? p['variables'] : []) {
      if (!isObject(v) || typeof v['name'] !== 'string') continue;
      const items = Array.isArray(v['enum'])
        ? v['enum'].flatMap((i) =>
            isObject(i) && typeof i['name'] === 'string' && typeof i['value'] === 'number'
              ? [{ name: i['name'], value: i['value'] }]
              : [],
          )
        : null;
      variables.push({
        name: v['name'],
        type: text(v['type']),
        role: text(v['role'], 'output'),
        writable: v['writable'] === true,
        unit: text(v['unit']),
        min: numberOrNull(v['min']),
        max: numberOrNull(v['max']),
        step: numberOrNull(v['step']),
        description: text(v['description']),
        enum: items,
      });
    }
    peers.push({ name: p['name'], connected: p['connected'] === true, variables });
  }
  return {
    session,
    status: text(value['state'], 'unknown'),
    reason: typeof value['reason'] === 'string' ? value['reason'] : null,
    peers,
    peersOmitted: count(value['peers_omitted']),
    variablesOmitted: count(value['variables_omitted']),
  };
}

/** `operation` の通知（と要約の operations の 1 件）を読む。読めなければ null。 */
export function readOperation(value: unknown, session: number): OperationView | null {
  if (
    !isObject(value) ||
    !Number.isSafeInteger(value['seq']) ||
    typeof value['state'] !== 'string'
  ) {
    return null;
  }
  const peers: Record<string, OperationPeer> = {};
  for (const [name, p] of Object.entries(isObject(value['peers']) ? value['peers'] : {})) {
    if (!isObject(p)) continue;
    peers[name] = {
      state: text(p['state'], 'unknown'),
      appliedAtMs: numberOrNull(p['applied_at_ms']),
      late: p['late'] === true,
      reason: typeof p['reason'] === 'string' ? p['reason'] : null,
    };
  }
  return {
    session,
    seq: value['seq'] as number,
    state: value['state'],
    done: value['done'] === true,
    peers,
  };
}

/** 変数 1 つの履歴: 大きさの決まった環状バッファー。 */
class History {
  private times: Float64Array;
  private values: Float64Array;
  /** 値の種類: 0 は数、1 は bool、2 は null。 */
  private kinds: Uint8Array;
  private capacity: number;
  private start = 0;
  private length = 0;

  /** limit: 覚える点の数の上限。場所は、点が来るたびに倍々で取る（先に全部は取らない）。 */
  constructor(private max: number) {
    this.capacity = Math.min(max, HISTORY_FIRST);
    this.times = new Float64Array(this.capacity);
    this.values = new Float64Array(this.capacity);
    this.kinds = new Uint8Array(this.capacity);
  }

  get size(): number {
    return this.length;
  }

  /** 取ってある場所（点の数）。 */
  get reserved(): number {
    return this.capacity;
  }

  get limit(): number {
    return this.max;
  }

  get lastTime(): number | null {
    return this.length === 0
      ? null
      : (this.times[(this.start + this.length - 1) % this.capacity] ?? null);
  }

  /**
   * 1 点足す。場所がいっぱいで、上限に届いていなければ、mayGrow(増やす点の数) が許す限り倍にする。許されなければ、
   * いまの大きさのまま古い点を捨てる。
   */
  push(sampleMs: number, value: VariableValue, mayGrow: (more: number) => boolean): void {
    if (this.max === 0) return;
    if (this.length === this.capacity && this.capacity < this.max) {
      const next = Math.min(this.max, this.capacity * 2);
      if (mayGrow(next - this.capacity)) this.resize(next);
    }
    const at = (this.start + this.length) % this.capacity;
    this.set(at, sampleMs, value);
    if (this.length < this.capacity) this.length += 1;
    else this.start = (this.start + 1) % this.capacity;
  }

  /** 最後の点の値を置き換える（同じステップの値が、後から違う値で届いたとき）。 */
  replaceLast(value: VariableValue): void {
    if (this.length === 0) return;
    const at = (this.start + this.length - 1) % this.capacity;
    this.set(at, this.times[at] ?? 0, value);
  }

  private set(at: number, sampleMs: number, value: VariableValue): void {
    this.times[at] = sampleMs;
    this.values[at] = value === null ? 0 : Number(value);
    this.kinds[at] = value === null ? 2 : typeof value === 'boolean' ? 1 : 0;
  }

  /** 新しいほうから最大 n 点を、古い順に。 */
  last(n: number): HistoryPoint[] {
    const take = Math.min(n, this.length);
    const out: HistoryPoint[] = [];
    for (let i = this.length - take; i < this.length; i++) {
      const at = (this.start + i) % this.capacity;
      const kind = this.kinds[at];
      const value = this.values[at] ?? 0;
      out.push({
        sampleMs: this.times[at] ?? 0,
        value: kind === 2 ? null : kind === 1 ? value !== 0 : value,
      });
    }
    return out;
  }

  /** 上限を上げる（場所は、点が来てから取る）。 */
  raise(max: number): void {
    if (max > this.max) this.max = max;
  }

  private resize(capacity: number): void {
    const kept = this.last(this.length);
    this.capacity = capacity;
    this.times = new Float64Array(capacity);
    this.values = new Float64Array(capacity);
    this.kinds = new Uint8Array(capacity);
    this.start = 0;
    this.length = kept.length;
    kept.forEach((point, at) => this.set(at, point.sampleMs, point.value));
  }
}

/** 1 つの watch（利用者が持つもの）の中身。 */
interface Handle {
  /** 見ている変数（null は全部）。 */
  keys: Set<string> | null;
  periodMs: number;
  history: number;
  values: Writable<WatchValues>;
}

/** サーバーへの購読 1 つ（間隔ごと）。 */
interface Bucket {
  id: number;
  periodMs: number;
  /** サーバーに最後に頼んだ内容（JSON。同じなら送り直さない）。null は、頼んでいない。 */
  asked: string | null;
}

/** 答えを待っている書き込み。 */
interface Pending {
  /** 書き込みを向けた実行（書いた時点の実行）と、操作の番号（HTTP の答えが来るまでは null）。 */
  session: number;
  seq: number | null;
  resolve: (operation: OperationView) => void;
  reject: (error: WriteError) => void;
  timer: ReturnType<typeof setTimeout> | undefined;
  /** HTTP の答え（番号）を待つ間に届いた、最後の要約の操作の部分（番号がわかったときに照らす）。 */
  summary?: Listed;
}

/** 要約の操作の部分: 一覧、境目（言わないサーバーでは null）、サーバーが覚えきれなかった未確定の操作があるか。 */
interface Listed {
  operations: readonly OperationView[];
  fromSeq: number | null;
  openOmitted: boolean;
}

/**
 * 要約から、その番号の操作の結果がもう届かないとわかれば、その誤り（わからなければ null）。要約の一覧には、
 * 未確定の操作の全部と、境目（operations_from_seq）以上の番号の操作の全部が入っている。だから、一覧になく、境目
 * より古い操作は、切れている間に確定していて、その通知はもう来ない。一覧にある操作と、境目以上の番号の操作は、
 * まだ届いていないだけなので待つ。境目を言わない要約では、見分けられないので待つ。
 *
 * サーバーが未確定の操作を覚えきれなかったとき（operations_open_omitted が 1 以上）は、一覧にない古い操作が確定済みか
 * 未確定かを見分けられない。サーバーはもう追っていないので、同じく「答えが来なかった」にする（文で分ける。
 * no-answer は「反映されたかどうかわからない」）。
 */
function lost(seq: number, summary: Listed): WriteError | null {
  if (summary.fromSeq === null || seq >= summary.fromSeq) return null;
  if (summary.operations.some((o) => o.seq === seq)) return null;
  return new WriteError(
    'no-answer',
    summary.openOmitted
      ? `the server does not keep track of the write (operation ${seq}) any more (it holds too many open ` +
          'operations): its result may not come; look at the values before writing again'
      : `the result of the write (operation ${seq}) was lost while the connection was down (it was settled ` +
          'meanwhile, and the server does not list it any more): look at the values before writing again',
  );
}

/** VariablesFeed が接続に頼むこと。 */
export interface VariablesLink {
  /** つながっていれば送る（つながっていなければ何もしない。つながったときに resume() の分を送る）。 */
  send(message: unknown): unknown;
  onBatch(listener: (batch: readonly unknown[]) => void): () => void;
}

/**
 * ECU の変数: 一覧と状態、値の watch、書き込みと結果。枠組みに依らない。
 *
 * - 一覧と状態は、hello と HTTP の要約の `variables` と、`variables` の通知から作る（`session` の通知には入って
 *   いない）。実行が替わったら、その実行の `variables` の通知が届くまで unknown。
 * - watch は、変数ごとに、頼まれた中でいちばん短い間隔の購読（間隔ごとに 1 つ）に入れる。購読は、変わったときに
 *   同じ id で送り直し（サーバーが置き換える）、つなぎ直したときは resume() で全部送り直す。
 * - 値は変数ごとに最後の 1 つと、頼まれた数だけの履歴（環状バッファー）を持つ。実行が替わったら捨てる。
 * - 書き込みの結果は、`operation` の通知で届く。HTTP の答え（操作の番号）より先に届くこともあるので、最近の操作を
 *   覚えておいて、番号がわかったところで照らす。
 */
export class VariablesFeed implements Omit<ClientVariables, 'bind'> {
  private readonly stateValue: Writable<VariablesState>;
  private readonly operationsValue: Writable<readonly OperationView[]>;
  private readonly handles = new Set<Handle>();
  private readonly buckets = new Map<number, Bucket>();
  private readonly readings = new Map<string, VariableReading>();
  private readonly histories = new Map<string, History>();
  private readonly pending = new Set<Pending>();
  /** だれかが見ている変数（null は、全部を見る watch がある）。 */
  private watched: Set<string> | null = new Set();
  /** 全部を見る watch が頼んだ履歴の点の数（いちばん多いもの）。 */
  private allHistory = 0;
  /** 名前で頼まれた履歴の点の数（変数ごとに、頼んだ watch のうち最も多いもの）。 */
  private namedHistory = new Map<string, number>();
  /** 履歴のために取ってある場所の合計（点の数）。 */
  private historyReserved = 0;
  private readonly problemValue: Writable<WatchProblem | null>;
  private nextId = 1;
  private syncQueued = false;

  readonly state: Readable<VariablesState>;
  readonly operations: Readable<readonly OperationView[]>;
  readonly watchProblem: Readable<WatchProblem | null>;

  constructor(
    private readonly link: VariablesLink,
    private readonly http: HttpTransport,
    private readonly onError: ErrorSink = rethrowLater,
    /** 上限（api/app の limits から。まだなければ既定）。 */
    private readonly limits: () => {
      watch_period_ms_min?: number;
      watch_period_ms_max?: number;
      watch_variables?: number;
    } = () => ({}),
  ) {
    this.stateValue = new Writable<VariablesState>(NO_VARIABLES, Object.is, onError);
    this.operationsValue = new Writable<readonly OperationView[]>([], Object.is, onError);
    this.problemValue = new Writable<WatchProblem | null>(null, Object.is, onError);
    this.state = this.stateValue.readonly();
    this.operations = this.operationsValue.readonly();
    this.watchProblem = this.problemValue.readonly();
    link.onBatch((batch) => this.receive(batch));
  }

  // --- 接続 -----------------------------------------------------------------------------------

  /** つないだ直後（認証の後）に送るメッセージ: いまの購読を全部。 */
  resume(): unknown[] {
    this.syncQueued = false;
    const messages: unknown[] = [];
    for (const [periodMs, wanted] of this.wanted()) {
      const bucket = this.bucket(periodMs);
      const message = this.watchMessage(bucket, wanted);
      bucket.asked = JSON.stringify(message);
      messages.push(message);
    }
    for (const [periodMs, bucket] of this.buckets) {
      if (!messages.some((m) => (m as { id: number }).id === bucket.id))
        this.buckets.delete(periodMs);
    }
    return messages;
  }

  /** クライアントを閉じた: 待っている書き込み（HTTP の答えを待っているものも）を断る。watch は残る。 */
  dispose(): void {
    for (const pending of [...this.pending]) {
      this.settle(
        pending,
        new WriteError('closed', 'the client was closed before the result came'),
      );
    }
  }

  // --- 通知 -----------------------------------------------------------------------------------

  private receive(batch: readonly unknown[]): void {
    const touched = new Set<string>();
    let touchedAll = false;
    let operations: OperationView[] | null = null;
    let fresh: OperationView[] = [];
    // 実行が替わる前に、それまでに届いた操作の結果を当てる（同じまとまりで届いた確定の結果を、切り替えで
    // 「わからない」にしない）
    const flush = () => {
      if (operations) this.setOperations(operations, fresh);
      operations = null;
      fresh = [];
    };
    for (const message of batch) {
      if (!isObject(message)) continue;
      switch (message['type']) {
        case 'hello': {
          flush();
          const summary = message['session'];
          if (isObject(summary) && Number.isSafeInteger(summary['id'])) {
            this.adoptSummary(summary);
          } else {
            this.enter(null);
            this.stateValue.set({ ...NO_VARIABLES, status: 'off' });
          }
          touchedAll = true;
          break;
        }
        case 'session': {
          const summary = message['session'];
          const id = isObject(summary) ? summary['id'] : null;
          // 別の実行が始まった: その変数の知らせ（variables）が続けて届く。それまでは unknown
          if (Number.isSafeInteger(id) && id !== this.stateValue.get().session) {
            flush();
            this.enter(id as number);
            this.stateValue.set({ ...NO_VARIABLES, session: id as number });
            touchedAll = true;
          }
          break;
        }
        case 'variables': {
          const id = message['session'];
          if (!Number.isSafeInteger(id)) break;
          if (id !== this.stateValue.get().session) {
            flush();
            this.enter(id as number);
            touchedAll = true;
          }
          this.stateValue.set(readVariables(message['variables'], id as number));
          break;
        }
        case 'values': {
          if (message['session'] !== this.stateValue.get().session) break;
          this.takeValues(message, touched);
          break;
        }
        case 'operation': {
          const id = message['session'];
          if (id !== this.stateValue.get().session) break;
          const operation = readOperation(message, id as number);
          if (operation) {
            operations = this.withOperation(operations ?? this.operationsValue.get(), operation);
            fresh.push(operation);
          }
          break;
        }
        case 'error': {
          // watch が断られた: その接続では、その購読の値は届かない
          const code = message['code'];
          if (code === 'invalid-watch' || code === 'too-many-watches') {
            this.problemValue.set({ code, message: text(message['message']) });
          }
          break;
        }
      }
    }
    flush();
    if (touchedAll || touched.size > 0) this.publish(touchedAll ? null : touched);
  }

  /** hello と HTTP の要約（`session`）の変数の部分を当てる。 */
  adoptSummary(summary: unknown): void {
    if (!isObject(summary) || !Number.isSafeInteger(summary['id'])) return;
    const id = summary['id'] as number;
    const variables = summary['variables'];
    if (id !== this.stateValue.get().session) {
      this.enter(id);
      this.publish(null);
    } else if (variables === undefined) {
      return; // 同じ実行の、変数の欄のない要約（session の通知）: いまのまま
    }
    this.stateValue.set(readVariables(variables, id));
    if (isObject(variables) && Array.isArray(variables['operations'])) {
      let operations = this.operationsValue.get();
      const listed: OperationView[] = [];
      for (const entry of variables['operations']) {
        const operation = readOperation(entry, id);
        if (operation) {
          listed.push(operation);
          operations = this.withOperation(operations, operation);
        }
      }
      this.setOperations(operations, listed);
      const fromSeq = variables['operations_from_seq'];
      const openOmitted = variables['operations_open_omitted'];
      this.forgetLost(id, {
        operations: listed,
        fromSeq: Number.isSafeInteger(fromSeq) ? (fromSeq as number) : null,
        openOmitted: typeof openOmitted === 'number' && openOmitted > 0,
      });
    }
  }

  /**
   * つなぎ直した要約（か api/session の要約）で、結果を待っている書き込みのうち、もう結果が届かないものを
   * 「答えが来なかった」で確定させる（lost）。まだ番号を知らない書き込み（HTTP の答えの前）は、要約を覚えておき、
   * 番号がわかったときに同じように照らす。
   */
  private forgetLost(session: number, summary: Listed): void {
    for (const pending of [...this.pending]) {
      if (pending.session !== session) continue;
      if (pending.seq === null) {
        pending.summary = summary;
        continue;
      }
      const error = lost(pending.seq, summary);
      if (error) this.settle(pending, error);
    }
  }

  /**
   * 別の実行に替わった: 前の実行の値、履歴、操作を捨てる（名前で頼まれていない履歴は、場所ごと捨てる: 実行ごとに
   * 変数が替わっても増え続けない）。前の実行の結果を待っている書き込みは、結果が来ないので断る。
   */
  private enter(session: number | null): void {
    if (session === this.stateValue.get().session) return;
    this.readings.clear();
    this.histories.clear();
    this.historyReserved = 0;
    this.operationsValue.set([]);
    for (const pending of [...this.pending]) {
      if (pending.session !== session) {
        this.settle(
          pending,
          new WriteError(
            'no-answer',
            `the run ${pending.session} is over (another one has started): the result of the write ` +
              (pending.seq === null ? '' : `(operation ${pending.seq}) `) +
              'did not come',
          ),
        );
      }
    }
  }

  private takeValues(message: Fields, touched: Set<string>): void {
    const peers = message['peers'];
    if (!isObject(peers)) return;
    const simMs = numberOrNull(message['sim_ms']);
    const paused = message['paused'] === true;
    const mayGrow = (more: number) => {
      if (this.historyReserved + more > HISTORY_TOTAL_MAX) return false;
      this.historyReserved += more;
      return true;
    };
    for (const [peer, entry] of Object.entries(peers)) {
      if (!isObject(entry) || !isObject(entry['values'])) continue;
      const sampleMs = numberOrNull(entry['sample_ms']) ?? 0;
      for (const [name, raw] of Object.entries(entry['values'])) {
        const value: VariableValue =
          typeof raw === 'boolean' || (typeof raw === 'number' && Number.isFinite(raw))
            ? raw
            : null;
        const key = keyOf(peer, name);
        // だれも見ていない変数（やめた直後に届いた分、「全部」で頼んだ購読の余り）は持たない
        if (this.watched !== null && !this.watched.has(key)) continue;
        const before = this.readings.get(key);
        if (
          before &&
          before.sampleMs === sampleMs &&
          before.simMs === simMs &&
          before.paused === paused &&
          Object.is(before.value, value)
        ) {
          continue;
        }
        this.readings.set(key, { value, sampleMs, simMs, paused });
        touched.add(key);
        // 履歴: 頼まれた数（名前で頼んだ watch と、全部を見る watch の多いほう）まで。場所は点が来てから取る
        const wanted = Math.max(this.namedHistory.get(key) ?? 0, this.allHistory);
        if (wanted === 0) continue;
        let history = this.histories.get(key);
        if (!history) {
          if (!mayGrow(Math.min(wanted, HISTORY_FIRST))) continue;
          history = new History(wanted);
          this.histories.set(key, history);
        } else {
          history.raise(wanted);
        }
        const last = history.lastTime;
        if (last === null || sampleMs > last) history.push(sampleMs, value, mayGrow);
        else if (sampleMs === last) history.replaceLast(value); // 同じステップの値が、変わって届いた
      }
    }
  }

  /** 変わった変数を見ている watch の values を作り直す（touched が null なら全部の watch）。 */
  private publish(touched: Set<string> | null): void {
    for (const handle of this.handles) {
      if (touched !== null && handle.keys !== null) {
        let hit = false;
        for (const key of handle.keys) {
          if (touched.has(key)) {
            hit = true;
            break;
          }
        }
        if (!hit) continue;
      }
      handle.values.set(this.valuesOf(handle));
    }
  }

  private valuesOf(handle: Handle): WatchValues {
    const out: Record<string, Record<string, VariableReading>> = {};
    const add = (key: string, reading: VariableReading) => {
      const cut = key.indexOf('\u0000');
      (out[key.slice(0, cut)] ??= {})[key.slice(cut + 1)] = reading;
    };
    if (handle.keys === null) {
      for (const [key, reading] of this.readings) add(key, reading);
    } else {
      for (const key of handle.keys) {
        const reading = this.readings.get(key);
        if (reading) add(key, reading);
      }
    }
    return out;
  }

  // --- 操作 -----------------------------------------------------------------------------------

  private withOperation(
    operations: readonly OperationView[],
    operation: OperationView,
  ): OperationView[] {
    const at = operations.findIndex((o) => o.seq === operation.seq);
    // 確定した操作は、もう変わらない（遅れて届いた古い状態で戻さない）
    if (at >= 0 && operations[at]?.done && !operation.done) return [...operations];
    const next =
      at >= 0 ? operations.map((o, i) => (i === at ? operation : o)) : [...operations, operation];
    return next.length > OPERATIONS_KEPT ? next.slice(next.length - OPERATIONS_KEPT) : next;
  }

  /**
   * 操作の一覧を替え、結果を待っている書き込みのうち、確定したものを片づける。`fresh` は、いま届いた操作
   * （通知のまとまり、要約の一覧）: 一覧は OPERATIONS_KEPT 件に切るので、切られて一覧に残らなかった確定も、ここ
   * から拾う。
   */
  private setOperations(
    operations: readonly OperationView[],
    fresh: readonly OperationView[] = [],
  ): void {
    this.operationsValue.set(operations);
    for (const pending of [...this.pending]) {
      if (pending.seq === null) continue;
      const mine = (o: OperationView) =>
        o.done && o.seq === pending.seq && o.session === pending.session;
      const operation = fresh.find(mine) ?? operations.find(mine);
      if (operation) this.settle(pending, operation);
    }
  }

  private settle(pending: Pending, outcome: OperationView | WriteError): void {
    if (!this.pending.delete(pending)) return;
    clearTimeout(pending.timer);
    if (outcome instanceof WriteError) {
      pending.reject(outcome);
    } else if (outcome.state === 'applied') {
      pending.resolve(outcome);
    } else {
      const reasons = Object.entries(outcome.peers)
        .filter(([, p]) => p.state !== 'applied')
        .map(([name, p]) => `${name}: ${p.state}${p.reason ? ` (${p.reason})` : ''}`);
      pending.reject(
        new WriteError(
          'failed',
          `the write (operation ${outcome.seq}) was not applied: ${reasons.join('; ') || outcome.state}`,
          null,
          outcome,
        ),
      );
    }
  }

  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options: WriteOptions = {},
  ): Promise<OperationView> {
    // 待つ時間: 無限大は時計なし（実行の終わりと close() で必ず片づく）。数でない、0 以下は既定。長すぎる値は、
    // 時計が受け取れる最長に丸める（そのまま渡すと、すぐに時間切れになる）
    const asked = options.timeoutMs;
    const timeoutMs =
      asked === Infinity
        ? Infinity
        : typeof asked === 'number' && asked > 0
          ? Math.min(asked, TIMER_MAX_MS)
          : WRITE_TIMEOUT_MS;
    // 書き込みは、いまの実行に向ける。サーバーは、その実行がもう動いていなければ断る（session-changed）
    const session = this.stateValue.get().session;
    if (session === null) {
      return Promise.reject(
        new WriteError(
          'refused',
          'there is no run to write to (wait for the variables of a run: client.variables.state)',
          'not-running',
        ),
      );
    }
    const body: Fields = { session, writes };
    if (options.atMs !== undefined) body['at_ms'] = options.atMs;
    if (options.late !== undefined) body['late'] = options.late;
    return new Promise<OperationView>((resolve, reject) => {
      const pending: Pending = { session, seq: null, resolve, reject, timer: undefined };
      this.pending.add(pending);
      const known = () =>
        this.operationsValue
          .get()
          .find((o) => o.seq === pending.seq && o.session === pending.session);
      this.http({ method: 'POST', path: 'api/write', body }, (outcome) => {
        if (!this.pending.has(pending)) return; // close() が先に断った
        if (!outcome.ok) {
          const error = outcome.error;
          const code = error instanceof HttpError ? (error.problem?.code ?? null) : null;
          // 答えがなかった（status 0）、Control Server が時間内に答えなかった（504）、途中で切れた（502）、
          // サーバーが途中で失敗した（500）: 受け取られたかもしれない。断られた（refused）のは、送っていないと
          // わかっているときだけ
          const unsure = error instanceof HttpError && [0, 500, 502, 504].includes(error.status);
          this.settle(
            pending,
            new WriteError(
              unsure ? 'no-answer' : 'refused',
              unsure
                ? `${errorMessage(error)} (the write may have been taken: look at the operations ` +
                    'and the values before writing again)'
                : errorMessage(error),
              code,
            ),
          );
          return;
        }
        const answer = outcome.body;
        const seq = isObject(answer) ? answer['operation'] : null;
        const answered = isObject(answer) ? answer['session_id'] : null;
        if (!Number.isSafeInteger(seq) || !Number.isSafeInteger(answered)) {
          this.settle(
            pending,
            new WriteError(
              'no-answer',
              'the server answered the write without an operation number',
            ),
          );
          return;
        }
        // 答えの前に実行が替わった（ふつうは enter() が先に断る）: その実行の結果は、もう届かない
        if (answered !== pending.session || this.stateValue.get().session !== pending.session) {
          this.settle(
            pending,
            new WriteError(
              'no-answer',
              `the run ${pending.session} ended before the answer to the write came (operation ${String(seq)}): ` +
                'its result will not come; look at the values before writing again',
            ),
          );
          return;
        }
        pending.seq = seq as number;
        if (options.onAccepted) {
          try {
            options.onAccepted({ session: pending.session, seq: pending.seq });
          } catch (error) {
            this.onError(error);
          }
          if (!this.pending.has(pending)) return; // 呼んだ先が close() した
        }
        if (timeoutMs !== Infinity) {
          pending.timer = setTimeout(() => {
            const last = known();
            this.settle(
              pending,
              new WriteError(
                'no-answer',
                `no result of the write (operation ${pending.seq}) came in ${timeoutMs / 1000} s` +
                  (last
                    ? `: it is ${last.state} (it is applied when the simulation reaches its step)`
                    : ''),
                null,
                last ?? null,
              ),
            );
          }, timeoutMs);
          // Node: この時計だけのために、プロセスを終わらせずに待たない
          (pending.timer as { unref?: () => void }).unref?.();
        }
        // 結果が HTTP の答えより先に届いていることがある（通知で。または、答えを待つ間につなぎ直した要約で:
        // その要約から、結果がもう届かないとわかることもある）
        const summary = pending.summary;
        const already =
          known() ??
          summary?.operations.find((o) => o.seq === seq && o.session === pending.session);
        if (already?.done) this.settle(pending, already);
        else if (summary) {
          const error = lost(pending.seq, summary);
          if (error) this.settle(pending, error);
        }
      });
    });
  }

  // --- watch ----------------------------------------------------------------------------------

  watch(variables: readonly VariableRef[] | 'all', options: WatchOptions = {}): VariableWatch {
    const history = options.history ?? 0;
    const handle: Handle = {
      keys: variables === 'all' ? null : new Set(variables.map((v) => keyOf(v.peer, v.name))),
      periodMs: this.period(options.periodMs ?? WATCH_PERIOD_MS),
      // 数でない、負は 0（覚えない）
      history: Number.isFinite(history)
        ? Math.max(0, Math.min(Math.floor(history), HISTORY_MAX))
        : 0,
      values: new Writable<WatchValues>({}, Object.is, this.onError),
    };
    this.handles.add(handle);
    this.forget();
    handle.values.set(this.valuesOf(handle));
    this.queueSync();
    let open = true;
    const mine = (ref: VariableRef) => {
      const key = keyOf(ref.peer, ref.name);
      return open && (handle.keys === null || handle.keys.has(key)) ? key : null;
    };
    return {
      values: handle.values.readonly(),
      value: (ref) => {
        const key = mine(ref);
        return key === null ? undefined : this.readings.get(key);
      },
      history: (ref) => {
        const key = mine(ref);
        return key === null ? [] : (this.histories.get(key)?.last(handle.history) ?? []);
      },
      close: () => {
        if (!open) return;
        open = false;
        this.handles.delete(handle);
        this.forget();
        this.queueSync();
      },
    };
  }

  /**
   * 頼まれた間隔を、WATCH_PERIODS_MS のうち頼まれた間隔以下で最も長いものにし、サーバーの範囲（下限と上限）に収める。
   */
  private period(wanted: number): number {
    const limits = this.limits();
    const min = limits.watch_period_ms_min ?? WATCH_PERIODS_MS[0];
    const max = limits.watch_period_ms_max ?? WATCH_PERIODS_MS[WATCH_PERIODS_MS.length - 1] ?? min;
    let chosen: number = WATCH_PERIODS_MS[0];
    for (const period of WATCH_PERIODS_MS) if (period <= wanted) chosen = period;
    return Math.min(Math.max(chosen, min), Math.max(max, min));
  }

  /** だれが何を見ているかを数え直し、だれも見なくなった変数の値と、だれも頼まなくなった履歴を捨てる。 */
  private forget(): void {
    let all = false;
    let allHistory = 0;
    const seen = new Set<string>();
    const named = new Map<string, number>();
    for (const handle of this.handles) {
      if (handle.keys === null) {
        all = true;
        allHistory = Math.max(allHistory, handle.history);
        continue;
      }
      for (const key of handle.keys) {
        seen.add(key);
        if (handle.history > 0) named.set(key, Math.max(named.get(key) ?? 0, handle.history));
      }
    }
    this.watched = all ? null : seen;
    this.allHistory = allHistory;
    this.namedHistory = named;
    for (const [key, history] of this.histories) {
      if (allHistory === 0 && !named.has(key)) {
        this.historyReserved -= history.reserved;
        this.histories.delete(key);
      }
    }
    if (all) return;
    for (const key of this.readings.keys()) if (!seen.has(key)) this.readings.delete(key);
  }

  /** 間隔ごとの、頼む変数（null は全部）。変数は、それを見る watch のうち、いちばん短い間隔のところに入れる。 */
  private wanted(): Map<number, Set<string> | null> {
    const fastest = new Map<string, number>();
    let allPeriod: number | null = null;
    for (const handle of this.handles) {
      if (handle.keys === null) {
        allPeriod = Math.min(allPeriod ?? Infinity, handle.periodMs);
        continue;
      }
      for (const key of handle.keys) {
        fastest.set(key, Math.min(fastest.get(key) ?? Infinity, handle.periodMs));
      }
    }
    const out = new Map<number, Set<string> | null>();
    if (allPeriod !== null) out.set(allPeriod, null);
    const limit = this.limits().watch_variables ?? WATCH_VARIABLES;
    for (const [key, periodMs] of fastest) {
      // 「全部」の購読が同じかもっと短い間隔であれば、そこで届く
      if (allPeriod !== null && allPeriod <= periodMs) continue;
      const keys = out.get(periodMs);
      if (keys === undefined) out.set(periodMs, new Set([key]));
      else keys?.add(key);
    }
    // 名前で挙げられる数を超えたら、その間隔は全部を頼む（受け取った後、見ている変数だけを使う）
    for (const [periodMs, keys] of out) if (keys && keys.size > limit) out.set(periodMs, null);
    return out;
  }

  private bucket(periodMs: number): Bucket {
    let bucket = this.buckets.get(periodMs);
    if (!bucket) {
      bucket = { id: this.nextId++, periodMs, asked: null };
      this.buckets.set(periodMs, bucket);
    }
    return bucket;
  }

  private watchMessage(bucket: Bucket, wanted: Set<string> | null): unknown {
    const message = (variables: unknown) => ({
      type: 'watch',
      id: bucket.id,
      variables,
      period_ms: bucket.periodMs,
    });
    if (wanted === null) return message('all');
    const named = message(
      [...wanted].sort().map((key) => {
        const cut = key.indexOf('\u0000');
        return { peer: key.slice(0, cut), name: key.slice(cut + 1) };
      }),
    );
    // 長い名前が多いと、1 通がサーバーの上限を超える（超えた 1 通で、サーバーは接続を閉じる）。そのときは全部を
    // 頼む（受け取った後、見ている変数だけを使う）
    return utf8Length(JSON.stringify(named)) > WATCH_MESSAGE_BYTES ? message('all') : named;
  }

  /** watch が増えたり減ったりした後、まとめて 1 回、サーバーの購読を合わせる。 */
  private queueSync(): void {
    if (this.syncQueued) return;
    this.syncQueued = true;
    queueMicrotask(() => {
      if (!this.syncQueued) return;
      this.syncQueued = false;
      this.sync();
    });
  }

  private sync(): void {
    const wanted = this.wanted();
    for (const [periodMs, bucket] of [...this.buckets]) {
      if (wanted.has(periodMs)) continue;
      this.buckets.delete(periodMs);
      if (bucket.asked !== null) this.link.send({ type: 'unwatch', id: bucket.id });
    }
    for (const [periodMs, keys] of wanted) {
      const bucket = this.bucket(periodMs);
      const message = this.watchMessage(bucket, keys);
      const asked = JSON.stringify(message);
      if (asked === bucket.asked) continue;
      bucket.asked = asked;
      this.problemValue.set(null); // 頼み直した: 前に断られたことは、もう当たらない
      this.link.send(message);
    }
  }
}
