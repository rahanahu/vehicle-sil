import { Injectable, computed, inject, signal } from '@angular/core';
import { ActionError, CheckResult, Intent } from '@vehicle-sil/gui-client';
import { GUI_FRONT, readableSignal } from './gui-client';

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
  readonly stopping = readableSignal(this.control.stopping);
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = readableSignal(this.control.stopWhileStarting);

  private readonly failure = signal<{ error: ActionError; run: string } | null>(null);
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。実行の状態が変わったら出さない。 */
  readonly error = computed(() => {
    const failure = this.failure();
    return failure && failure.run === this.status().run ? failure.error.message : null;
  });

  /**
   * `file` を `intent` で照合する（正面の口の `check`: 1 つずつ送り、同じ意図の照合は分け合う。`signal` が中断した
   * 照合は、待っている間なら送らない）。失敗は呼んだ側のもの（error には出さない）。
   */
  check(intent: Intent, file: string, signal?: AbortSignal): Promise<CheckResult> {
    return this.front.check(intent, { file, ...(signal ? { signal } : {}) });
  }

  /** `file` の実行を `intent` で始める。断られたら error に出る。 */
  start(intent: Intent, file: string): void {
    this.send(() => this.front.run.start(intent, { file }));
  }

  /** 順に止める（participant の終わりを待つ）。 */
  stop(): void {
    this.send(() => this.front.run.stop());
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): void {
    this.send(() => this.front.run.forceStop());
  }

  private send(operation: () => Promise<void>): void {
    this.failure.set(null);
    operation().catch((error: unknown) => {
      if (error instanceof ActionError) {
        this.failure.set({ error, run: this.status().run });
      }
    });
  }
}
