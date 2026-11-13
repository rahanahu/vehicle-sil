import { DestroyRef, Injectable, Signal, computed, inject, signal } from '@angular/core';
import {
  HistoryPoint,
  OperationView,
  VariableInfo,
  VariableReading,
  VariableRef,
  VariableWatch,
  WatchOptions,
  WatchValues,
  WriteOptions,
} from '@vehicle-sil/gui-client';
import { GUI_CLIENT, readableSignal } from './gui-client';
import { ServerConnection } from './server-connection';

/** 部品が持つ変数の watch: 値は signal。部品（注入の文脈）が壊れると、自分でやめる。 */
export interface WatchedVariables {
  /** watch している変数の最後の値（`values()[peer]?.[name]`）。 */
  readonly values: Signal<WatchValues>;
  /** 変数の最後の値（まだなければ undefined）。signal を読むので、template と computed の中で使える。 */
  value(ref: VariableRef): VariableReading | undefined;
  /** 変数の履歴（古い順）。値が届くたびに変わるので、template と computed の中で使える。 */
  history(ref: VariableRef): HistoryPoint[];
  /** 早めにやめる（部品が壊れる前に）。 */
  close(): void;
}

/**
 * ECU の変数の、Angular の包み: 一覧と状態、最近の書き込みの結果を signal で出し、値の watch と書き込みを
 * クライアントのライブラリ（`@vehicle-sil/gui-client` の `client.variables`）へ渡す。アプリ全体で 1 つ。
 *
 * 中身はライブラリにある（同じ変数の watch をまとめる、つなぎ直しで購読し直す、履歴、書き込みの結果を待つ）。
 * ここは signal にするだけ。画面の部品は、ここの signal と watch() だけを使う。
 */
@Injectable({ providedIn: 'root' })
export class EcuVariables {
  private readonly client = inject(GUI_CLIENT).variables;
  // 接続を始めさせる（変数の状態は WebSocket で届く）
  private readonly server = inject(ServerConnection);
  private readonly current = readableSignal(this.client.state);

  /** どの実行のものか（実行がなければ null）。 */
  readonly session = computed(() => this.current().session);
  /** 変数の機能の状態: unknown、off、connecting、ready、failed、ended。 */
  readonly status = computed(() => this.current().status);
  /** 使えない、つながらない理由（サーバーの英語の文。なければ null）。 */
  readonly reason = computed(() => this.current().reason);
  /** 変数を持つ participant と、その変数（宣言の順）。 */
  readonly peers = computed(() => this.current().peers);
  /** 読み書きできるか（状態が ready）。 */
  readonly ready = computed(() => this.current().status === 'ready');
  /** 上限を超えたのでサーバーが覚えていない変数の数。 */
  readonly omitted = computed(() => this.current().variablesOmitted);
  /** サーバーが watch を断ったこと（ふつうは null）。 */
  readonly watchProblem = readableSignal(this.client.watchProblem);
  /** 最近の書き込みの結果（古い順）。 */
  readonly operations: Signal<readonly OperationView[]> = readableSignal(this.client.operations);

  /** participant の名前と変数の名前から、定義と participant がつながっているか（多くの部品が引くので、表にしておく）。 */
  private readonly table = computed(() => {
    const table = new Map<string, Map<string, VariableInfo>>();
    for (const peer of this.peers()) {
      table.set(peer.name, new Map(peer.variables.map((variable) => [variable.name, variable])));
    }
    return table;
  });
  private readonly connectedPeers = computed(
    () =>
      new Set(
        this.peers()
          .filter((peer) => peer.connected)
          .map((peer) => peer.name),
      ),
  );

  /** 変数の定義（いまの実行になければ undefined）。signal を読むので、template と computed の中で使える。 */
  find(ref: VariableRef): VariableInfo | undefined {
    return this.table().get(ref.peer)?.get(ref.name);
  }

  /** その participant が、いま Control Server とつながっているか。 */
  peerConnected(peer: string): boolean {
    return this.connectedPeers().has(peer);
  }

  /**
   * 変数の値を受け取り始める。注入の文脈（部品の field の初期化、constructor）で呼ぶ: その部品が壊れると、
   * watch もやめる。
   */
  watch(variables: readonly VariableRef[] | 'all', options?: WatchOptions): WatchedVariables {
    // 先に注入の文脈を確かめる: 文脈の外で呼ばれたら、watch を作る前に投げる（作った watch が残らない）
    const destroyed = inject(DestroyRef);
    const watch = this.open(variables, options);
    destroyed.onDestroy(() => watch.close());
    return watch;
  }

  /**
   * watch() と同じだが、注入の文脈が要らず、部品が壊れても自分ではやめない。必ず close() する。見えている間だけ
   * watch する部品（`variables/widgets/variable-binding.ts`）のように、後から作り直すときに使う。
   */
  open(variables: readonly VariableRef[] | 'all', options?: WatchOptions): WatchedVariables {
    const watch: VariableWatch = this.client.watch(variables, options);
    const values = signal(watch.values.get());
    const stop = watch.values.subscribe((next) => values.set(next));
    return {
      values: values.asReadonly(),
      value: (ref) => values()[ref.peer]?.[ref.name],
      history: (ref) => {
        values(); // 値が届くたびに読み直させる
        return watch.history(ref);
      },
      close: () => {
        stop();
        watch.close();
      },
    };
  }

  /**
   * 変数を書く。反映されたらその操作で解け、ほかは WriteError で断る（kind: refused、failed、no-answer、closed）。
   */
  write(
    writes: Record<string, Record<string, number | boolean | string>>,
    options?: WriteOptions,
  ): Promise<OperationView> {
    return this.client.write(writes, options);
  }
}
