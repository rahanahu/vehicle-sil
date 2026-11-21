import { Injectable, computed, inject } from '@angular/core';
import { ActionError, CheckResult, Intent, OperationName } from '@vehicle-sil/gui-client';
import { GUI_FRONT, lastOperation, readableSignal } from './gui-client';

/**
 * 実行の照合と操作（開始、順に止める、待たずに止める）の、Angular の包み。アプリ全体で 1 つ。操作の可否と理由は
 * 正面の口の `controls`、照合と操作は正面の口の `check` と `run`（Promise）で、ここは自分で可否を推測せず、意図を
 * 組み直さない。応答の要約は、ライブラリがセッションの写しに当てる。
 */
@Injectable({ providedIn: 'root' })
export class RunCommands {
  private readonly front = inject(GUI_FRONT);
  private readonly control = this.front.low.run;

  /** 各操作を押せるか、押せない理由（正面の口が言う）。 */
  readonly controls = readableSignal(this.front.controls);
  /** 実行の状態と、終わった実行の結果（正面の口の `status`）。 */
  readonly status = readableSignal(this.front.status);

  /** 開始を送って、応答を待っている。 */
  readonly starting = readableSignal(this.control.starting);
  /** 停止を送って、応答を待っている（どちらの停止か）。 */
  readonly stopping = readableSignal(this.control.currentStopping);
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = computed(() => this.starting() && this.stopping() !== null);

  private readonly last = lastOperation(this.status, ['start', 'stop', 'force_stop']);
  /**
   * 最後の操作がこのアプリの開始か停止なら、それと正面の口の `status.problem`（いつ消えるかは正面の口が決める: 次の
   * 操作か、`status.run` が変わったとき）。それ以外は null。
   */
  readonly failure = this.last.failure;

  /**
   * `file` を `intent` で照合する（正面の口の `check`: 1 つずつ送り、同じ意図の照合は分け合う。`signal` が中断した
   * 照合は、待っている間なら送らない。`fresh` なら、送っている最中の照合は分け合わない）。失敗は呼んだ側のもの
   * （`status.problem` には出ない）。
   */
  check(
    intent: Intent,
    file: string,
    options: { signal?: AbortSignal; fresh?: boolean } = {},
  ): Promise<CheckResult> {
    return this.front.check(intent, { file, ...options });
  }

  /** `file` の実行を `intent` で始める。断られたら `status.problem` に出る。 */
  start(intent: Intent, file: string): void {
    this.send('start', () => this.front.run.start(intent, { file }));
  }

  /** 順に止める（participant の終わりを待つ）。 */
  stop(): void {
    this.send('stop', () => this.front.run.stop());
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(target?: { session: number; instance: string }): void {
    this.send('force_stop', () => this.front.run.forceStop(target));
  }

  private send(op: OperationName, operation: () => Promise<void>): void {
    this.last.sending(op);
    operation().catch((error: unknown) => {
      // 失敗は正面の口が `status.problem` に置く
      if (!(error instanceof ActionError)) throw error;
    });
  }
}
