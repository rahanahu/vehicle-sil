import { Injectable, computed, inject, signal } from '@angular/core';
import { ActionError, Intent, RunRequest } from '@vehicle-sil/gui-client';
import { GUI_FRONT, readableSignal } from './gui-client';

/**
 * 実行の操作（開始、順に止める、待たずに止める）の、Angular の包み。アプリ全体で 1 つ。操作の可否と理由は正面の口の
 * `controls`、操作は正面の口の `run`（Promise）で、ここは自分で可否を推測しない。応答の要約は、ライブラリが
 * セッションの写しに当てる。
 */
@Injectable({ providedIn: 'root' })
export class RunCommands {
  private readonly front = inject(GUI_FRONT);
  private readonly control = this.front.low.run;

  /** 各操作を押せるか、押せない理由（正面の口が言う）。 */
  readonly controls = readableSignal(this.front.controls);
  private readonly status = readableSignal(this.front.status);

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

  /** 実行を始める。断られたら error に出る。 */
  start(request: RunRequest): void {
    const intent: Intent = {
      disable: request.disable,
      start_paused: request.start_paused,
      ...(request.manual ? { manual: request.manual } : {}),
      ...(request.scenario !== undefined ? { scenario: request.scenario } : {}),
      ...(request.speed !== null && request.speed !== undefined ? { speed: request.speed } : {}),
    };
    this.send(() => this.front.run.start(intent, { file: request.file }));
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
