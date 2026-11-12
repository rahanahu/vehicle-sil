import { Injectable, inject } from '@angular/core';
import { RunRequest } from '@vehicle-sil/gui-client';
import { GUI_CLIENT, readableSignal } from './gui-client';

/**
 * 実行の操作（開始、順に止める、待たずに止める）の、Angular の包み。アプリ全体で 1 つ。中身はクライアントの
 * ライブラリ（`@vehicle-sil/gui-client` の RunControl）。応答の要約は、ライブラリがセッションの写しに当てる。
 */
@Injectable({ providedIn: 'root' })
export class RunCommands {
  private readonly control = inject(GUI_CLIENT).run;

  /** 開始を送って、応答を待っている。 */
  readonly starting = readableSignal(this.control.starting);
  /** 停止を送って、応答を待っている（どちらの停止か）。 */
  readonly stopping = readableSignal(this.control.stopping);
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = readableSignal(this.control.stopWhileStarting);
  /** 最後の操作が断られた理由（サーバーの文をそのまま）。実行が始まるか終わったら null。 */
  readonly error = readableSignal(this.control.error);

  /** 実行を始める。断られたら error に出る。 */
  start(request: RunRequest): void {
    this.control.start(request).catch(() => undefined);
  }

  /** 順に止める（participant の終わりを待つ）。 */
  stop(): void {
    this.control.stop().catch(() => undefined);
  }

  /** 待たずに止める（participant を強制終了する）。 */
  forceStop(): void {
    this.control.forceStop().catch(() => undefined);
  }
}
