import { Injectable, computed, inject } from '@angular/core';
import { CommandView, SendResult, TimeCommand, TimeView } from '@vehicle-sil/gui-client';
import { GUI_CLIENT, readableSignal } from './gui-client';
import { ServerConnection } from './server-connection';
import { ServerInfo } from './server-info';

export { ANSWER_TIMEOUT } from './gui-client';

/**
 * シミュレーションの時刻の、Angular の包み: いまの仮想時間、速さ、一時停止、実際の速さ、遅れの数、最後の命令の結果を
 * signal で出し、時刻の命令（一時停止、再開、ステップ送り、速さの変更）を GUI サーバーへ送る。アプリ全体で 1 つ。
 *
 * - 中身はクライアントのライブラリ（`@vehicle-sil/gui-client` の TimeControl）。ここは、その状態を signal にし、
 *   答えが来なかった知らせを画面の言葉にするだけ。
 * - 画面の部品はここの signal を読むだけにする（どの部品からも同じ状態が見える）。
 */
@Injectable({ providedIn: 'root' })
export class SimulationTime {
  private readonly control = inject(GUI_CLIENT).time;
  // 接続と api/app の読み込みを始めさせる（状態はセッションの写しとランチャーの features から作る）
  private readonly server = inject(ServerConnection);
  private readonly info = inject(ServerInfo);
  private readonly state = readableSignal(this.control.state);

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
  /** 時刻の命令を送れるか（ランチャーが対応し、段階が running で、結果がまだで、止めるよう頼んでいない）。 */
  readonly controllable = computed(() => this.state().controllable);
  /**
   * 最初のステップの前で、再開かステップ送りを待っているか（止めて始めた実行で、ランチャーが変数を読み書きする口を
   * 持つとき）。この間は time がまだないが、再開とステップ送りを押せる。
   */
  readonly beforeStart = computed(() => this.state().beforeStart);
  /** 送って、まだ答えを見ていない命令。その間は、次の命令を送らない。 */
  readonly sending = computed(() => this.state().sending);
  /** ステップ送りの途中（ランチャーは一時停止の扱い）。 */
  readonly stepping = computed(() => this.state().stepping);
  /** 一時停止を押せるか。 */
  readonly canPause = computed(() => this.state().canPause);
  /** 再開を押せるか。 */
  readonly canResume = computed(() => this.state().canResume);
  /** ステップ送りを押せるか。 */
  readonly canStep = computed(() => this.state().canStep);
  /**
   * 最後の命令がうまくいかなかった理由: サーバーが断った（英語のまま）か、ランチャーが答えを待つ時間の間に答え
   * なかった（画面の文）。実行が変わったら出さない。
   */
  readonly error = computed(() => {
    const failure = this.state().failure;
    if (!failure) return null;
    if (failure.kind === 'server') return failure.message;
    return $localize`ランチャーが命令（${failure.command}:command:）に ${this.control.answerTimeoutMs / 1000}:seconds: 秒たっても答えません。入力を読んでいないかもしれません。もう一度送れます。`;
  });
  /** error がサーバーの文（英語）か。 */
  readonly errorFromServer = computed(() => this.state().failure?.kind === 'server');

  pause(): SendResult {
    return this.control.pause();
  }

  resume(): SendResult {
    return this.control.resume();
  }

  /** 一時停止しているとき、仮想時間を ms 進めて、また止める（ランチャーが刻みの倍数に切り上げる）。 */
  step(ms: number): SendResult {
    return this.control.step(ms);
  }

  /** 速さを変える（一時停止はそのまま）。 */
  setSpeed(value: number | 'max'): SendResult {
    return this.control.setSpeed(value);
  }

  /**
   * 命令を送る。前の命令の答えを待っている間と、実行がないときは送らず、`{sent: false, reason}` を返す。
   * 押せるかは、送る前に canPause などで確かめる。
   */
  send(command: TimeCommand): SendResult {
    return this.control.send(command);
  }
}
