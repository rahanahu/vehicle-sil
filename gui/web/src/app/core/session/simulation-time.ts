import { Injectable, computed, inject } from '@angular/core';
import {
  ActionError,
  CommandView,
  OperationName,
  RunTarget,
  TimeView,
  isHolding,
} from '@vehicle-sil/gui-client';
import { ANSWER_TIMEOUT, GUI_FRONT, lastOperation, readableSignal } from './gui-client';
import { ServerConnection } from './server-connection';
import { ServerInfo } from './server-info';

export { ANSWER_TIMEOUT } from './gui-client';

/**
 * シミュレーションの時刻の、Angular の包み: いまの仮想時間、速さ、一時停止、実際の速さ、遅れの数、最後の命令の結果を
 * signal で出し、時刻の命令（一時停止、再開、ステップ送り、速さの変更）を GUI サーバーへ送る。アプリ全体で 1 つ。
 *
 * - 中身はクライアントのライブラリ（`@vehicle-sil/gui-client`）。操作の可否と理由は正面の口の `controls`、命令は
 *   正面の口の `time`（Promise）で、ここは自分で可否を推測しない。仮想時間と速さは正面の口の `clock`、一時停止は
 *   ランチャーの `state`（セッションの写しの `launcherState`）、遅れの数と最後の命令は `front.low.time`、命令の
 *   失敗は正面の口の `status.problem`。
 * - 画面の部品はここの signal を読むだけにする（どの部品からも同じ状態が見える）。
 */
@Injectable({ providedIn: 'root' })
export class SimulationTime {
  private readonly front = inject(GUI_FRONT);
  /** 時刻の命令の答えを待つ時間（ミリ秒。これを過ぎると `no-answer` で失敗する）。 */
  readonly answerTimeoutMs = inject(ANSWER_TIMEOUT);
  // 接続と api/app の読み込みを始めさせる（範囲はサーバーの limits から）
  private readonly server = inject(ServerConnection);
  private readonly info = inject(ServerInfo);
  private readonly control = this.front.low.time;
  private readonly state = readableSignal(this.control.state);
  private readonly session = readableSignal(this.front.low.session.session);
  readonly target = computed(() => {
    this.session();
    return this.front.low.session.currentTarget();
  });

  /** 各操作を押せるか、押せない理由、範囲（ステップ送りの上限、速さの範囲）。正面の口が言う。 */
  readonly controls = readableSignal(this.front.controls);
  private readonly status = readableSignal(this.front.status);
  /** 仮想時間と速さ（正面の口の `clock`。状態の帯もこれを読む: 帯とカードで値がずれない）。 */
  readonly clock = readableSignal(this.front.clock);

  /** いまのセッションの時刻（time の事象が来るまでは null）。 */
  readonly time = computed<TimeView | null>(() => this.state().time);
  /** 仮想時間（ミリ秒）。正面の口の `clock`（一時停止中は、ランチャーの `state` が言う止まった時刻も見る）。 */
  readonly simMs = computed(() => this.clock().sim_ms);
  /** いまの速さ（数か 'max'。正面の口の `clock`。ランチャーがまだ言っていなければ null）。 */
  readonly speed = computed(() => this.clock().speed);
  /**
   * 一時停止しているか（実行中のランチャーの `state` の `paused`。まだなければ null）。止めて始めた実行の、参加を待つ間と、
   * 一時停止中に停止を頼んだ後も true（`status.run` はそのとき `waiting`・`stopping` で、一時停止と言わない）。
   */
  readonly paused = computed(() => {
    const session = this.session();
    return session?.launcher.running ? (session.launcherState?.paused ?? null) : null;
  });
  /** 直近 1 秒ほどの実際の速さ。正面の口の `clock`（一時停止中は null。最後の time の値は止まる前のもの）。 */
  readonly actualSpeed = computed(() => this.clock().actual_speed);
  /** 速さを合わせている（数の速さ）。最速では遅れを数えないので、遅れの欄は出さない。 */
  readonly paced = computed(() => this.state().paced);
  /** 遅れの数（ランチャーが書かないときは null）。 */
  readonly lateness = computed(() => this.state().lateness);
  /** 実時間に遅れたか（許容を超えて遅れた刻みがある）。 */
  readonly behind = computed(() => this.state().behind);
  /** 最後に届いた command の事象（stop も含む）。 */
  readonly lastCommand = computed<CommandView | null>(() => this.state().lastCommand);
  /**
   * 最初のステップの前で、再開かステップ送りを待っているか（止めて始めた実行で、ランチャーが ECU の変数との接続を
   * 持つとき）。この間は time がまだないが、再開とステップ送りを押せる。ランチャーの `state` の `holding`。
   */
  readonly beforeStart = computed(() => isHolding(this.session()));
  /** 送って、まだ答えを見ていない命令がある（正面の口の `busy`）。 */
  readonly waiting = computed(() => {
    const c = this.controls();
    return [c.pause, c.resume, c.step, c.speed].some((x) => x.reason?.code === 'busy');
  });

  private readonly last = lastOperation(this.status, ['pause', 'resume', 'step', 'speed']);
  /**
   * 最後の操作がこのアプリの時刻の命令なら、それと正面の口の `status.problem`（いつ消えるかは正面の口が決める: 次の
   * 操作か、`status.run` が変わったとき）。それ以外は null。
   */
  readonly failure = this.last.failure;

  /** 一時停止を送る。失敗は `status.problem` に出る（この Promise は断られても失敗しない）。 */
  pause(target?: RunTarget | null): Promise<void> {
    return this.send('pause', () => this.front.time.pause(target));
  }

  resume(target?: RunTarget | null): Promise<void> {
    return this.send('resume', () => this.front.time.resume(target));
  }

  /** 一時停止しているとき、仮想時間を ms 進めて、また止める（ランチャーが刻みの倍数に切り上げる）。 */
  step(ms: number, target?: RunTarget | null): Promise<void> {
    return this.send('step', () => this.front.time.step(ms, target));
  }

  /** 速さを変える（一時停止はそのまま）。 */
  setSpeed(value: number | 'max', target?: RunTarget | null): Promise<void> {
    return this.send('speed', () => this.front.time.setSpeed(value, target));
  }

  private async send(op: OperationName, operation: () => Promise<void>): Promise<void> {
    this.last.sending(op);
    try {
      await operation();
    } catch (error) {
      // 失敗は正面の口が `status.problem` に置く
      if (!(error instanceof ActionError)) throw error;
    }
  }
}
