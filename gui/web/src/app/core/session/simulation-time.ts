import { Injectable, computed, inject, signal } from '@angular/core';
import { ActionError, CommandView, TimeView } from '@vehicle-sil/gui-client';
import { ANSWER_TIMEOUT, GUI_FRONT, readableSignal } from './gui-client';
import { ServerConnection } from './server-connection';
import { ServerInfo } from './server-info';

export { ANSWER_TIMEOUT } from './gui-client';

/**
 * シミュレーションの時刻の、Angular の包み: いまの仮想時間、速さ、一時停止、実際の速さ、遅れの数、最後の命令の結果を
 * signal で出し、時刻の命令（一時停止、再開、ステップ送り、速さの変更）を GUI サーバーへ送る。アプリ全体で 1 つ。
 *
 * - 中身はクライアントのライブラリ（`@vehicle-sil/gui-client`）。操作の可否と理由は正面の口の `controls`、命令は
 *   正面の口の `time`（Promise）で、ここは自分で可否を推測しない。見せるための状態（仮想時間、遅れ）は `front.low.time`。
 * - 画面の部品はここの signal を読むだけにする（どの部品からも同じ状態が見える）。
 */
@Injectable({ providedIn: 'root' })
export class SimulationTime {
  private readonly front = inject(GUI_FRONT);
  private readonly answerTimeoutMs = inject(ANSWER_TIMEOUT);
  // 接続と api/app の読み込みを始めさせる（状態はセッションの写しとランチャーの features から作る）
  private readonly server = inject(ServerConnection);
  private readonly info = inject(ServerInfo);
  private readonly control = this.front.low.time;
  private readonly state = readableSignal(this.control.state);

  /** 各操作を押せるか、押せない理由、範囲（ステップ送りの上限、速さの範囲）。正面の口が言う。 */
  readonly controls = readableSignal(this.front.controls);
  private readonly status = readableSignal(this.front.status);

  /** ランチャーが時刻の命令（pause、resume、step、speed）を持つか。持たなければ操作を出さない。 */
  readonly supported = computed(() => this.state().supported);
  /** いまのセッションの時刻（time の事象が来るまでは null）。 */
  readonly time = computed<TimeView | null>(() => this.state().time);
  /** 仮想時間（ミリ秒）。 */
  readonly simMs = computed(() => this.time()?.simMs ?? null);
  /** いまの速さ（数か 'max'。ランチャーが書かなければ null）。 */
  readonly speed = computed(() => this.time()?.speed ?? null);
  /** 一時停止しているか（わからなければ null）。 */
  readonly paused = computed(() => this.time()?.paused ?? null);
  /** 直近 1 秒ほどの実際の速さ。 */
  readonly actualSpeed = computed(() => this.time()?.actualSpeed ?? null);
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
   * 持つとき）。この間は time がまだないが、再開とステップ送りを押せる。
   */
  readonly beforeStart = computed(() => this.state().beforeStart);
  /** 送って、まだ答えを見ていない命令がある（正面の口の `busy`）。 */
  readonly waiting = computed(() => {
    const c = this.controls();
    return [c.pause, c.resume, c.step, c.speed].some((x) => x.reason?.code === 'busy');
  });

  private readonly failure = signal<{ error: ActionError; command: string; run: string } | null>(
    null,
  );
  /**
   * 最後の命令がうまくいかなかった理由: 理由の表にあれば画面の文、答えが来なかったなら画面の文、それ以外は
   * サーバーかランチャーの英語の文。実行の状態が変わったら出さない。
   */
  readonly error = computed(() => {
    const failure = this.failure();
    if (!failure || failure.run !== this.status().run) return null;
    const { error, command } = failure;
    if (error.code === 'no-answer') {
      return $localize`ランチャーが命令（${command}:command:）に ${this.answerTimeoutMs / 1000}:seconds: 秒たっても答えません。入力を読んでいないかもしれません。もう一度送れます。`;
    }
    return error.message;
  });
  /** error がサーバーかランチャーの文（英語）か。 */
  readonly errorFromServer = computed(() => {
    const failure = this.failure();
    return !!failure && failure.run === this.status().run && failure.error.code !== 'no-answer';
  });

  /** 一時停止を送る。失敗は error に出る（この Promise は断られても失敗しない）。 */
  pause(): Promise<void> {
    return this.send('pause', () => this.front.time.pause());
  }

  resume(): Promise<void> {
    return this.send('resume', () => this.front.time.resume());
  }

  /** 一時停止しているとき、仮想時間を ms 進めて、また止める（ランチャーが刻みの倍数に切り上げる）。 */
  step(ms: number): Promise<void> {
    return this.send('step', () => this.front.time.step(ms));
  }

  /** 速さを変える（一時停止はそのまま）。 */
  setSpeed(value: number | 'max'): Promise<void> {
    return this.send('speed', () => this.front.time.setSpeed(value));
  }

  private async send(command: string, operation: () => Promise<void>): Promise<void> {
    this.failure.set(null);
    try {
      await operation();
    } catch (error) {
      if (!(error instanceof ActionError)) throw error;
      // 停止のために途中で終わったステップ送りは、誤りではない
      if (error.code === 'interrupted' || error.code === 'not-running') return;
      this.failure.set({ error, command, run: this.status().run });
    }
  }
}
