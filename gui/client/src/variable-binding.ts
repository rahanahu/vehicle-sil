import { ConnectionState } from './connection';
import { SessionView } from './session-state';
import { TimeState } from './time';
import { ErrorSink, Readable, Writable, callEach } from './values';
import {
  ValueCondition,
  WriteBlock,
  WriteStatus,
  WriteValue,
  hasRange,
  sameCondition,
  valueCondition,
  writeBlock,
} from './variable-state';
import {
  HistoryPoint,
  OperationView,
  VariableInfo,
  VariableReading,
  VariableRef,
  VariableWatch,
  VariablesState,
  WatchOptions,
  WriteError,
  WriteOptions,
} from './variables';

// 変数 1 つと画面の部品 1 つのつながり（client.variables.bind）。値の状態、書けるかどうかと理由、書き込みの様子を
// 1 つの読める値にまとめ、書き込みを変数ごと・実行ごとに 1 つにまとめる。枠組みに依らない。

/** client.variables.bind の設定。 */
export interface BindOptions {
  /** 値を受け取る間隔（ミリ秒。既定は WATCH_PERIOD_MS）。 */
  periodMs?: number;
  /** 覚えておく履歴の点の数（グラフ。既定 0）。 */
  history?: number;
  /** 範囲（min と max）がない変数には書かせない（スライダー）。合わない間は block が `no-range`。 */
  needsRange?: boolean;
  /** 値（読んだ値か送った値）がない間は書かせない（値から動かす部品: スライダー）。その間は block が `no-value`。 */
  needsValue?: boolean;
  /**
   * 最初から値を受け取るか（既定 true）。false なら、setVisible(true) を呼ぶまで watch しない（画面に見えている
   * 部品だけが値を受け取るようにするとき）。
   */
  visible?: boolean;
}

/** 変数 1 つの、いまの様子（VariableBinding.state の値）。 */
export interface BoundVariable {
  /** 変数の名指し。 */
  readonly ref: VariableRef;
  /** 変数の定義（いまの実行になければ undefined）。 */
  readonly info: VariableInfo | undefined;
  /** 最後の値（まだない、受け取っていない（visible が偽）ときは undefined）。 */
  readonly reading: VariableReading | undefined;
  /** 値の状態（古い、まだない、止まっている時刻の値、など）。 */
  readonly condition: ValueCondition;
  /** いま書けない理由（書けるなら null）。 */
  readonly block: WriteBlock | null;
  /** この変数への最後の書き込みの様子（いまの実行の）。同じ変数の bind は、同じ様子を見る。 */
  readonly writeStatus: WriteStatus;
  /**
   * 送っていて結果がまだの値（なければ null）。部品は、これがあれば読んだ値の代わりに出す（押したスイッチが、
   * 結果を待つ間に元へ戻って見えないように）。実行が替わると消える。
   */
  readonly pending: WriteValue | null;
}

/**
 * 変数 1 つとのつながり。使い終わったら close() する。
 *
 * ```js
 * const throttle = client.variables.bind({ peer: 'Engine', name: 'engine.throttle' }, { needsRange: true });
 * throttle.state.subscribe(({ reading, pending, block }) => {
 *   slider.disabled = block !== null;
 *   slider.value = pending ?? reading?.value ?? 0;
 * });
 * slider.onchange = () => throttle.write(Number(slider.value));
 * ```
 */
export interface VariableBinding {
  /** 変数の名指し。 */
  readonly ref: VariableRef;
  /** いまの様子。get() は、いつもいまの状態から作る（購読の順に依らない）。 */
  readonly state: Readable<BoundVariable>;
  /** 履歴（古い順。BindOptions.history を頼み、値を受け取っているときだけ。ほかは空）。呼ぶたびに新しい配列。 */
  history(): HistoryPoint[];
  /**
   * 書く。書けないとき（state の block）は何もしない。前の書き込みをサーバーがまだ受け取っていなければ、送らずに
   * 覚えておき、受け取ったら最後の値だけを送る（スライダーやキーの連打で、書き込みが積み重ならない。そのとき、
   * 実行が同じで、まだ書けるかを確かめ直し、だめなら送らずに writeStatus が `dropped`）。結果が来なかった書き込みを、
   * 自分で送り直すことはしない。結果は state の writeStatus に出る（Promise は返さない）。
   */
  write(value: WriteValue): void;
  /**
   * 送る前に部品が見つけた誤り（数でない文字、範囲の外など）を、書き込みの様子（`invalid`）として出す。送っている
   * 書き込み、結果を待っている書き込み、覚えている次の値には触れない。
   */
  reject(message: string): void;
  /** 値を受け取るか（見えている間だけ watch する部品が、見え隠れのたびに呼ぶ）。偽の間、reading は undefined。 */
  setVisible(visible: boolean): void;
  /** やめる（watch と購読を片づける）。この後の write、reject、setVisible は何もしない。 */
  close(): void;
}

/** 覚えた値を送らなかったときの文（WriteStatus の dropped）。 */
export const DROPPED_MESSAGE =
  'the value kept back was not sent: the variable cannot be written any more ' +
  '(the run ended or another one started, or the ECU is not connected). ' +
  'Check the value and write it again';

/** bind が読むもの（クライアントの状態）。 */
export interface BindingSources {
  readonly variables: {
    readonly state: Readable<VariablesState>;
    watch(variables: readonly VariableRef[], options?: WatchOptions): VariableWatch;
    write(
      writes: Record<string, Record<string, number | boolean | string>>,
      options?: WriteOptions,
    ): Promise<OperationView>;
  };
  readonly connection: Readable<ConnectionState>;
  readonly time: Readable<TimeState>;
  readonly session: Readable<SessionView | null>;
  readonly onError: ErrorSink;
}

/** 送った書き込み 1 つ（どの書き込みかを、オブジェクトそのもので比べる）。 */
interface Sent {
  readonly value: WriteValue;
}

/**
 * 1 つの実行の、1 つの変数への書き込みの様子。書き込み 1 つは、送った（sending）→ 受け取られた（結果待ち）→
 * 確定、と進む。受け取られる前に断られたら、そこで確定する。
 *
 * - `sending`: サーバーがまだ受け取っていない書き込み（多くて 1 つ）。ある間、次の値は送らずに `queued` に覚える。
 *   消すのは、その書き込みの受け付けか確定だけ。
 * - `last`: 最後に送った書き込みで、まだ確定していないもの。`pending`（部品が出す「送った値」）の持ち主。消すのは、
 *   その書き込みの確定だけ（新しく送れば、そちらに替わる）。
 * - `shown`: `status`（様子の欄）が追っている書き込み。新しく送るか覚えると替わり、送る前の誤り（reject）と
 *   「送らなかった」（dropped）は、だれも追わない（null）にして自分の文を出す。追われていない書き込みの受け付けと
 *   確定は、様子の欄を変えない。
 *
 * 送る前の誤りは `shown` と `status` だけを変える。`sending`、`queued`、`last`、`pending` には触れない。
 */
interface Entry {
  session: number;
  key: string;
  ref: VariableRef;
  status: WriteStatus;
  pending: WriteValue | null;
  sending: Sent | null;
  queued: { value: WriteValue } | null;
  last: Sent | null;
  shown: Sent | null;
}

const keyOf = (ref: VariableRef) => `${ref.peer}\u0000${ref.name}`;

const IDLE: WriteStatus = { kind: 'idle' };

/**
 * bind の中身: 変数の定義の表引きと、書き込みの様子。クライアントに 1 つ。同じ変数を何か所で bind しても
 * 書き込みは重ならず、bind を作り直しても、送っている途中・失敗の様子は消えない。様子は実行ごとで、実行が替わると
 * 空から始まる（前の実行の「反映した」や「送った値」を、新しい実行に出さない）。
 */
export class VariableBinder {
  private entries = new Map<string, Entry>();
  private entriesSession: number | null = null;
  /** 変数ごとの、書き込みの様子が変わったときに呼ぶ関数（bind したものが聞く）。 */
  private readonly listeners = new Map<string, Set<() => void>>();
  /** 一覧の表（peers が替わったら作り直す）。 */
  private tableOf: VariablesState['peers'] | null = null;
  private table = new Map<string, Map<string, VariableInfo>>();
  private connectedPeers = new Set<string>();

  constructor(readonly sources: BindingSources) {}

  /** 変数とつなぐ（ClientVariables.bind）。 */
  bind(ref: VariableRef, options: BindOptions = {}): VariableBinding {
    return new Binding(this, { peer: ref.peer, name: ref.name }, options);
  }

  private lookUp(): void {
    const peers = this.sources.variables.state.get().peers;
    if (peers === this.tableOf) return;
    this.tableOf = peers;
    this.table = new Map();
    this.connectedPeers = new Set();
    for (const peer of peers) {
      this.table.set(peer.name, new Map(peer.variables.map((v) => [v.name, v])));
      if (peer.connected) this.connectedPeers.add(peer.name);
    }
  }

  /** 変数の定義（いまの実行になければ undefined）。 */
  find(ref: VariableRef): VariableInfo | undefined {
    this.lookUp();
    return this.table.get(ref.peer)?.get(ref.name);
  }

  /** その participant が、いま Control Server とつながっているか。 */
  peerConnected(peer: string): boolean {
    this.lookUp();
    return this.connectedPeers.has(peer);
  }

  /** クライアントと GUI サーバーの接続が開いているか。 */
  connected(): boolean {
    return this.sources.connection.get() === 'open';
  }

  /** いま書けない理由（書けるなら null）。部品に依らない分だけ（シナリオ、範囲、値の有無は Binding が足す）。 */
  block(ref: VariableRef): WriteBlock | null {
    return writeBlock({
      info: this.find(ref),
      status: this.sources.variables.state.get().status,
      connected: this.connected(),
      beforeStart: this.sources.time.get().beforeStart,
      peerConnected: this.peerConnected(ref.peer),
    });
  }

  /** いまの実行での、その変数への最後の書き込みの様子。 */
  status(ref: VariableRef): WriteStatus {
    return this.entry(ref)?.status ?? IDLE;
  }

  /** 送っていて結果がまだの値（なければ null）。 */
  pending(ref: VariableRef): WriteValue | null {
    return this.entry(ref)?.pending ?? null;
  }

  /** その変数の書き込みの様子が変わったら呼ぶ。返す関数でやめる。 */
  onChange(ref: VariableRef, listener: () => void): () => void {
    const key = keyOf(ref);
    const set = this.listeners.get(key) ?? new Set<() => void>();
    this.listeners.set(key, set);
    set.add(listener);
    return () => {
      set.delete(listener);
      if (set.size === 0 && this.listeners.get(key) === set) this.listeners.delete(key);
    };
  }

  private changed(entry: Entry): void {
    const set = this.listeners.get(entry.key);
    if (set) callEach(set, [], this.sources.onError);
  }

  /**
   * 書く。書けないときは何もしない。前の書き込みをサーバーがまだ受け取っていなければ、送らずに覚えておき、
   * 受け取ったら最後の値だけを送る（そのとき、実行が同じで、まだ書けるかを確かめ直す）。結果が来なかった
   * 書き込みを、自分で送り直すことはしない。
   */
  write(ref: VariableRef, value: WriteValue): void {
    if (this.block(ref) !== null) return;
    const entry = this.entry(ref);
    if (!entry) return;
    if (entry.sending) {
      // 様子の欄は、送っている書き込みを追う（断られたら、覚えた値も送らないので、その理由を出す）
      entry.queued = { value };
      entry.shown = entry.sending;
      entry.status = { kind: 'sending' };
      entry.pending = value;
      this.changed(entry);
      return;
    }
    this.send(entry, value);
  }

  /**
   * 送る前に部品が見つけた誤り（数でない文字、範囲の外など）を、書き込みの様子として出す。送っている書き込み、
   * 結果を待っている書き込み、覚えている次の値には触れない（すでに送った書き込みの結果は、様子の欄には出ないが、
   * 送った値の表示は確定で消える。覚えている値が後で送られたら、様子の欄はその書き込みを追う）。
   */
  reject(ref: VariableRef, message: string): void {
    const entry = this.entry(ref);
    if (!entry) return;
    entry.shown = null;
    entry.status = { kind: 'invalid', message };
    this.changed(entry);
  }

  /** いまの実行の、その変数の様子（実行がなければ undefined）。実行が替わったら、前の実行の様子を捨てる。 */
  private entry(ref: VariableRef): Entry | undefined {
    const session = this.sources.variables.state.get().session;
    if (session !== this.entriesSession) {
      this.entries = new Map();
      this.entriesSession = session;
    }
    if (session === null) return undefined;
    const key = keyOf(ref);
    let entry = this.entries.get(key);
    if (!entry) {
      entry = {
        session,
        key,
        ref: { peer: ref.peer, name: ref.name },
        status: IDLE,
        pending: null,
        sending: null,
        queued: null,
        last: null,
        shown: null,
      };
      this.entries.set(key, entry);
    }
    return entry;
  }

  private send(entry: Entry, value: WriteValue): void {
    const mine: Sent = { value };
    const { peer, name } = entry.ref;
    entry.sending = mine;
    entry.last = mine;
    entry.shown = mine;
    entry.status = { kind: 'sending' };
    entry.pending = value;
    this.changed(entry);
    /** サーバーが受け取った: 覚えていた次の値があれば送る。 */
    const accepted = (seq: number | null) => {
      if (entry.sending !== mine) return;
      entry.sending = null;
      const next = entry.queued;
      entry.queued = null;
      if (!next) {
        if (entry.shown === mine && seq !== null) {
          entry.status = { kind: 'queued', seq };
          this.changed(entry);
        }
        return;
      }
      // 覚えていた値: 同じ実行で、まだ書けるときだけ送る。variables.write は、実行が替わった後には onAccepted を
      // 呼ばない約束なので、same はここでいつも真になる（その約束が変わったときの守りとして残す）
      const same = this.sources.variables.state.get().session === entry.session;
      if (same && this.block(entry.ref) === null) {
        this.send(entry, next.value);
        return;
      }
      // 送らない。出していたのは送らなかった値なので消す（受け取られた mine の結果は、様子の欄には出さない）
      entry.pending = null;
      entry.shown = null;
      entry.status = { kind: 'dropped', message: DROPPED_MESSAGE };
      this.changed(entry);
    };
    /** 確定した（反映、失敗、断られた、答えなし）: もう結果は来ない。 */
    const settled = (status: WriteStatus) => {
      if (entry.sending === mine) {
        // 受け取られる前に終わった: 覚えていた次の値も送らない（同じ理由で断られる）
        entry.sending = null;
        entry.queued = null;
      }
      if (entry.last === mine) {
        entry.last = null;
        entry.pending = null;
      }
      if (entry.shown === mine) entry.status = status;
      this.changed(entry);
    };
    this.sources.variables
      .write(
        { [peer]: { [name]: value } },
        {
          // 止まっている間は、進めるまで結果が来ない。時間で切らずに、操作の結果を待つ（実行の終わり、実行の
          // 替わり、つなぎ直しで結果が失われたときは、write が「答えが来なかった」で確定させる）
          timeoutMs: Infinity,
          onAccepted: ({ seq }) => accepted(seq),
        },
      )
      .then(
        (operation) => {
          accepted(null); // ふつうは onAccepted が先に済ませている
          settled({ kind: 'applied', atMs: operation.peers[peer]?.appliedAtMs ?? null });
        },
        (error: unknown) =>
          settled(
            error instanceof WriteError
              ? { kind: error.kind, message: error.message, code: error.code }
              : { kind: 'no-answer', message: String(error), code: null },
          ),
      );
  }
}

function sameBound(a: BoundVariable, b: BoundVariable): boolean {
  return (
    a.info === b.info &&
    a.reading === b.reading &&
    a.block === b.block &&
    a.writeStatus === b.writeStatus &&
    a.pending === b.pending &&
    sameCondition(a.condition, b.condition)
  );
}

/** bind 1 つ。クライアントの状態と書き込みの様子が変わるたびに、state を作り直す。 */
class Binding implements VariableBinding {
  readonly state: Readable<BoundVariable>;
  private readonly value: Writable<BoundVariable>;
  private readonly stops: (() => void)[] = [];
  private watch: VariableWatch | null = null;
  private stopWatch: (() => void) | null = null;
  private open = true;

  constructor(
    private readonly binder: VariableBinder,
    readonly ref: VariableRef,
    private readonly options: BindOptions,
  ) {
    const { sources } = binder;
    if (options.visible ?? true) this.openWatch();
    this.value = new Writable(this.compute(), sameBound, sources.onError);
    const refresh = () => this.refresh();
    this.stops.push(
      sources.variables.state.subscribe(refresh),
      sources.connection.subscribe(refresh),
      sources.time.subscribe(refresh),
      sources.session.subscribe(refresh),
      binder.onChange(ref, refresh),
    );
    this.state = {
      // 読むたびに、いまの状態から作る（ほかの購読者が先に呼ばれても、古い様子を返さない）
      get: () => {
        this.refresh();
        return this.value.get();
      },
      subscribe: (listener) => this.value.subscribe(listener),
    };
  }

  private refresh(): void {
    if (this.open) this.value.set(this.compute());
  }

  private compute(): BoundVariable {
    const { binder, ref } = this;
    const { sources } = binder;
    const variables = sources.variables.state.get();
    const info = binder.find(ref);
    const reading = this.watch?.value(ref);
    const condition = valueCondition({
      status: variables.status,
      known: info !== undefined,
      reading,
      connected: binder.connected(),
      peerConnected: binder.peerConnected(ref.peer),
      // ready のまま理由があるのは、値の読み取りが失敗し続けているとき（値は最後に読めたもの）
      readFailing: variables.status === 'ready' && variables.reason !== null,
    });
    const pending = binder.pending(ref);
    return {
      ref,
      info,
      reading,
      condition,
      block: this.blockOf(info, reading, pending),
      writeStatus: binder.status(ref),
      pending,
    };
  }

  private blockOf(
    info: VariableInfo | undefined,
    reading: VariableReading | undefined,
    pending: WriteValue | null,
  ): WriteBlock | null {
    const base = this.binder.block(this.ref);
    if (base !== null) return base;
    // シナリオの実行中は見るだけ（書き込みの様子は持たない。サーバーも断る）
    const session = this.binder.sources.session.get();
    if (session?.scenario && session.launcher.running) return 'scenario';
    if (this.options.needsRange && info && !hasRange(info)) return 'no-range';
    if (this.options.needsValue && reading === undefined && pending === null) return 'no-value';
    return null;
  }

  private openWatch(): void {
    const { periodMs, history } = this.options;
    const watch = this.binder.sources.variables.watch([this.ref], { periodMs, history });
    this.watch = watch;
    this.stopWatch = watch.values.subscribe(() => this.refresh());
  }

  private closeWatch(): void {
    this.stopWatch?.();
    this.stopWatch = null;
    this.watch?.close();
    this.watch = null;
  }

  history(): HistoryPoint[] {
    return this.options.history && this.watch ? this.watch.history(this.ref) : [];
  }

  write(value: WriteValue): void {
    if (!this.open) return;
    if (this.compute().block !== null) return;
    this.binder.write(this.ref, value);
  }

  reject(message: string): void {
    if (this.open) this.binder.reject(this.ref, message);
  }

  setVisible(visible: boolean): void {
    if (!this.open || visible === (this.watch !== null)) return;
    if (visible) this.openWatch();
    else this.closeWatch();
    this.refresh();
  }

  close(): void {
    if (!this.open) return;
    this.open = false;
    for (const stop of this.stops) stop();
    this.closeWatch();
  }
}
