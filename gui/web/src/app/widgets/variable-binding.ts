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
  BoundVariable,
  HistoryPoint,
  ValueCondition,
  VariableInfo,
  VariableReading,
  VariableRef,
  VariableValue,
  WriteBlock,
  WriteStatus,
  WriteValue,
  VariableBinding as ClientBinding,
} from '@vehicle-sil/gui-client';
import { GUI_CLIENT } from '../core/session/gui-client';
import { ServerConnection } from '../core/session/server-connection';
import { VALUE_WORDS } from './labels';
import { formatValue } from './variable-state';
import { Visibility } from './visibility';

export type { WriteValue } from '@vehicle-sil/gui-client';

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
 * 値を watch するのは、部品が見えている間だけ。書き込みの様子は変数ごとに 1 つで、同じ変数の部品は同じ様子を見る。
 *
 * 中身はクライアントのライブラリ（`@vehicle-sil/gui-client` の `client.variables.bind`）。ここは、その様子を signal に
 * し、部品が見えているかを伝え、値を画面の言葉にするだけ。
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
 * 部品と変数をつなぐ。部品のコンポーネントの field の初期化で呼ぶ（注入の文脈が要る。部品が壊れるとつながりを
 * 閉じる）。`ref` は部品の input（signal）。1 つの部品で何度呼んでもよい（2 つの変数を描く部品など）。
 *
 * ```ts
 * readonly ref = input.required<VariableRef>();
 * protected readonly variable = bindVariable(this.ref);
 * ```
 */
export function bindVariable(ref: () => VariableRef, options: BindOptions = {}): VariableBinding {
  const variables = inject(GUI_CLIENT).variables;
  // 接続を始めさせる（変数の状態は WebSocket で届く）
  inject(ServerConnection);
  const host = inject(ElementRef<HTMLElement>);
  const visibility = inject(Visibility);

  // 同じ変数を指す新しいオブジェクトでは、つながりを作り直さない
  const current = computed(ref, { equal: sameRef });
  const visible = signal(false);
  inject(DestroyRef).onDestroy(
    visibility.observe(host.nativeElement, (shown) => visible.set(shown)),
  );
  const historyAsked = computed(() => {
    const asked = options.history;
    return typeof asked === 'function' ? asked() : (asked ?? 0);
  });

  // ライブラリのつながりは外の世界（購読）なので、effect で作り、閉じる。変数か履歴の数が変わったら作り直す
  // （書き込みの様子はライブラリが変数ごとに持つので、作り直しても消えない）。部品の effect は template より先に
  // 走るので、template が最初の effect の前の様子（下の UNKNOWN、IDLE、block は not-ready）を見ることはない
  const bound = signal<ClientBinding | null>(null);
  const state = signal<BoundVariable | null>(null);
  effect((onCleanup) => {
    const target = current();
    const history = historyAsked();
    const opened = untracked(() =>
      variables.bind(target, {
        periodMs: options.periodMs,
        history,
        needsRange: options.needsRange,
        needsValue: options.needsValue,
        visible: visible(),
      }),
    );
    const stop = opened.state.subscribe((next) => state.set(next));
    bound.set(opened);
    state.set(opened.state.get());
    onCleanup(() => {
      stop();
      opened.close();
    });
  });
  // 見えている間だけ watch する
  effect(() => {
    const opened = bound();
    const shown = visible();
    untracked(() => opened?.setVisible(shown));
  });

  const info = computed(() => state()?.info);
  return {
    ref: current,
    info,
    reading: computed(() => state()?.reading),
    history: computed(() => {
      state(); // 値が届くたびに読み直させる
      return historyAsked() ? (bound()?.history() ?? []) : [];
    }),
    condition: computed(() => state()?.condition ?? UNKNOWN),
    block: computed(() => {
      const now = state();
      return now ? now.block : 'not-ready';
    }),
    writeStatus: computed(() => state()?.writeStatus ?? IDLE),
    pending: computed(() => state()?.pending ?? null),
    id: `variable-${nextId++}`,
    text: (value) => {
      if (value === undefined) return '—';
      if (typeof value === 'string') return value;
      return formatValue(info(), value, VALUE_WORDS);
    },
    write: (value) => untracked(bound)?.write(value),
    reject: (message) => untracked(bound)?.reject(message),
  };
}

// 最初の effect の前（つながりがまだない）の様子
const UNKNOWN: ValueCondition = { kind: 'unavailable', status: 'unknown' };
const IDLE: WriteStatus = { kind: 'idle' };
