import {
  DestroyRef,
  ElementRef,
  Signal,
  computed,
  effect,
  inject,
  signal,
  untracked,
} from '@angular/core';
import {
  HistoryPoint,
  VariableInfo,
  VariableReading,
  VariableRef,
  VariableValue,
} from '@vehicle-sil/gui-client';
import { EcuVariables, WatchedVariables } from '../core/session/ecu-variables';
import { ServerConnection } from '../core/session/server-connection';
import { hasRange } from './dashboard-spec';
import { VALUE_WORDS } from './labels';
import {
  ValueCondition,
  WriteBlock,
  WriteStatus,
  formatValue,
  valueCondition,
} from './variable-state';
import { VariableWrites, WriteValue } from './variable-writes';
import { Visibility } from './visibility';

export type { WriteValue } from './variable-writes';

/** bindVariable の設定。 */
export interface BindOptions {
  /** 値を受け取る間隔（ミリ秒。既定はライブラリの既定 100）。 */
  periodMs?: number;
  /** 覚えておく履歴の点の数（グラフ。既定 0）。関数なら、変わったときに watch を作り直す。 */
  history?: number | (() => number);
  /** 範囲（min と max）がない変数には書かせない（スライダー）。 */
  needsRange?: boolean;
  /** 値（読んだ値か送った値）がない間は書かせない（値から動かす部品: スライダー）。 */
  needsValue?: boolean;
}

/**
 * 部品 1 つと変数 1 つのつながり。部品はこれだけを読めば、値、状態、書き込みを出せる（自分で作る部品も同じ）。
 * 値を watch するのは、部品が見えている間だけ。書き込みの様子は変数ごとに 1 つで（VariableWrites）、同じ変数の
 * 部品は同じ様子を見る。
 */
export interface VariableBinding {
  /** 変数の名指し。 */
  readonly ref: Signal<VariableRef>;
  /** 変数の定義（いまの実行になければ undefined）。 */
  readonly info: Signal<VariableInfo | undefined>;
  /** 最後の値（まだない、見えていないときは undefined）。 */
  readonly reading: Signal<VariableReading | undefined>;
  /** 履歴（古い順。BindOptions.history を頼んだときだけ）。 */
  readonly history: Signal<HistoryPoint[]>;
  /** 値の状態（古い、まだない、止まっている時刻の値、など）。 */
  readonly condition: Signal<ValueCondition>;
  /** いま書けない理由（書けるなら null）。 */
  readonly block: Signal<WriteBlock | null>;
  /** この変数への最後の書き込みの様子（いまの実行の）。 */
  readonly writeStatus: Signal<WriteStatus>;
  /**
   * 送っていて結果がまだの値（なければ null）。部品は、これがあれば読んだ値の代わりに出す（押したスイッチが、
   * 結果を待つ間に元へ戻って見えないように）。実行が替わると消える。
   */
  readonly pending: Signal<WriteValue | null>;
  /** 部品ごとに違う、要素の id の頭（aria の結びつけに使う）。 */
  readonly id: string;
  /**
   * 値を文字にする（単位なし。bool はオン・オフ、列挙は項目の名前）。undefined（値がない）は「—」、null（有限で
   * ない実数）は「数でない値」。
   */
  text(value: WriteValue | VariableValue | undefined): string;
  /**
   * 書く。書けないとき（block）は何もしない。前の書き込みをサーバーがまだ受け取っていなければ、送らずに覚えて
   * おき、受け取ったら最後の値だけを送る（スライダーやキーの連打で、書き込みが積み重ならない）。結果が来なかった
   * 書き込みを、自分で送り直すことはしない。
   */
  write(value: WriteValue): void;
  /** 送る前に部品が見つけた誤り（範囲の外など）を、書き込みの様子として出す。 */
  reject(message: string): void;
}

let nextId = 0;

const sameRef = (a: VariableRef, b: VariableRef) => a.peer === b.peer && a.name === b.name;

/**
 * 部品と変数をつなぐ。部品のコンポーネントの field の初期化で呼ぶ（注入の文脈が要る。部品が壊れると watch を
 * やめる）。`ref` は部品の input（signal）。1 つの部品で何度呼んでもよい（2 つの変数を描く部品など）。
 *
 * ```ts
 * readonly ref = input.required<VariableRef>();
 * protected readonly variable = bindVariable(this.ref);
 * ```
 */
export function bindVariable(ref: () => VariableRef, options: BindOptions = {}): VariableBinding {
  const variables = inject(EcuVariables);
  const writes = inject(VariableWrites);
  const server = inject(ServerConnection);
  const host = inject(ElementRef<HTMLElement>);
  const visibility = inject(Visibility);

  // 同じ変数を指す新しいオブジェクトでは、watch を作り直さない
  const current = computed(ref, { equal: sameRef });
  const visible = signal(false);
  inject(DestroyRef).onDestroy(
    visibility.observe(host.nativeElement, (shown) => visible.set(shown)),
  );
  const historyAsked = computed(() => {
    const asked = options.history;
    return typeof asked === 'function' ? asked() : (asked ?? 0);
  });

  // 見えている間だけ watch する。ライブラリの watch は外の世界（購読）なので、effect で作り、やめる
  const watch = signal<WatchedVariables | null>(null);
  effect((onCleanup) => {
    const target = current();
    const history = historyAsked();
    if (!visible()) return;
    const opened = untracked(() =>
      variables.open([target], { periodMs: options.periodMs, history }),
    );
    watch.set(opened);
    onCleanup(() => {
      opened.close();
      watch.set(null);
    });
  });

  const info = computed(() => variables.find(current()));
  const reading = computed(() => watch()?.value(current()));
  const history = computed(() => (historyAsked() ? (watch()?.history(current()) ?? []) : []));
  const condition = computed(() =>
    valueCondition({
      status: variables.status(),
      known: info() !== undefined,
      reading: reading(),
      connected: server.connectionState() === 'open',
      peerConnected: variables.peerConnected(current().peer),
    }),
  );
  const pending = computed(() => writes.pending(current()));
  const block = computed<WriteBlock | null>(() => {
    const base = writes.block(current());
    if (base !== null) return base;
    // シナリオの実行中は見るだけ（書き込みの様子は持たない。サーバーも断る）
    const session = server.session();
    if (session?.scenario && session.launcher.running) return 'scenario';
    const known = info();
    if (options.needsRange && known && !hasRange(known)) return 'no-range';
    if (options.needsValue && reading() === undefined && pending() === null) return 'no-value';
    return null;
  });

  return {
    ref: current,
    info,
    reading,
    history,
    condition,
    block,
    writeStatus: computed(() => writes.status(current())),
    pending,
    id: `variable-${nextId++}`,
    text: (value) => {
      if (value === undefined) return '—';
      if (typeof value === 'string') return value;
      return formatValue(info(), value, VALUE_WORDS);
    },
    write: (value) => {
      if (untracked(block) !== null) return;
      writes.write(untracked(current), value);
    },
    reject: (message) => writes.reject(untracked(current), message),
  };
}
