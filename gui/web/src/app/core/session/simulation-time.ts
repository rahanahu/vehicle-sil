import { HttpClient } from '@angular/common/http';
import { DestroyRef, Injectable, InjectionToken, computed, inject, signal } from '@angular/core';
import { TimeCommand, TimeCommandName, errorMessage } from '../api';
import { ServerConnection } from './server-connection';
import { ServerInfo } from './server-info';
import { CommandView, TimeView } from './session-state';

/** ランチャーの features のうち、時刻にかかわるもの。 */
export const FEATURE_SPEED = 'speed';
export const FEATURE_START_PAUSED = 'start-paused';
export const FEATURE_TIME_COMMANDS = 'time-commands';

/**
 * 送った命令へのランチャーの答え（command の事象）を待つ時間（ミリ秒）。過ぎたら待つのをやめて、操作を押せるように
 * 戻し、答えが来ないことを誤りとして出す（入力を読まないランチャー）。
 */
export const ANSWER_TIMEOUT_MS = 5000;

/** ANSWER_TIMEOUT_MS を差し替える口（単体試験が短くする）。 */
export const ANSWER_TIMEOUT = new InjectionToken<number>('ANSWER_TIMEOUT', {
  factory: () => ANSWER_TIMEOUT_MS,
});

/** 送らなかった理由: 実行がない、前の命令の答えを待っている。 */
export type NotSentReason = 'no-session' | 'waiting';

/** send() などの結果: 送ったか。送らなかったなら、その理由。 */
export type SendResult = { sent: true } | { sent: false; reason: NotSentReason };

/** 時刻の命令がうまくいかなかったこと。実行が変わったら出さない。 */
interface TimeFailure {
  /** server: サーバーが断った（文は英語のまま）、no-answer: ランチャーが答えなかった（画面の文）。 */
  kind: 'server' | 'no-answer';
  message: string;
  session: number | null;
}

/** 送って、ランチャーの答え（command の事象）をまだ見ていない命令。 */
interface Pending {
  command: TimeCommandName;
  session: number;
  /** 送ったときの最後の command の事象の seq。これより後の command の事象が答え。 */
  afterSeq: number;
}

/** 遅れの数（等速などで速さを合わせているときだけ意味がある）。 */
export interface Lateness {
  lateSteps: number;
  maxLateMs: number;
  slippedMs: number;
  lateMs: number;
}

/**
 * シミュレーションの時刻: いまの仮想時間、速さ、一時停止、実際の速さ、遅れの数、最後の命令の結果を signal で出し、
 * 時刻の命令（一時停止、再開、ステップ送り、速さの変更）を GUI サーバーへ送る。アプリ全体で 1 つ。
 *
 * - 状態はアプリ全体の接続（ServerConnection）のセッションから作る（サーバーの要約とランチャーの事象。要約が正）。
 *   途中から開いた画面にも、要約で同じ状態が出る。
 * - 命令はサーバーがランチャーに渡して、すぐ返る。受け付けたかは lastCommand、効いたことは time の signal に出る。
 * - 画面の部品はここの signal を読むだけにする（どの部品からも同じ状態が見える）。
 */
@Injectable({ providedIn: 'root' })
export class SimulationTime {
  private readonly http = inject(HttpClient);
  private readonly server = inject(ServerConnection);
  private readonly info = inject(ServerInfo);

  private readonly answerTimeoutMs = inject(ANSWER_TIMEOUT);
  private answerTimer: ReturnType<typeof setTimeout> | undefined;
  private readonly inFlight = signal(0);
  private readonly pending = signal<Pending | null>(null);
  private readonly failure = signal<TimeFailure | null>(null);

  /** ランチャーが時刻の命令（pause、resume、step、speed）を持つか。持たなければ操作を出さない。 */
  readonly supported = computed(() => this.info.features().includes(FEATURE_TIME_COMMANDS));

  /** いまのセッションの時刻（time の事象が来るまでは null）。 */
  readonly time = computed<TimeView | null>(() => this.server.session()?.time ?? null);
  /** 仮想時間（ミリ秒）。 */
  readonly simMs = computed(() => this.time()?.simMs ?? null);
  /** いまの速さ（数か 'max'。ランチャーが書かなければ null）。 */
  readonly speed = computed(() => this.time()?.speed ?? null);
  /** 一時停止しているか（わからなければ null）。 */
  readonly paused = computed(() => this.time()?.paused ?? null);
  /** 直近 1 秒ほどの実際の速さ。 */
  readonly actualSpeed = computed(() => this.time()?.actualSpeed ?? null);
  /** 速さを合わせている（数の速さ）。最速では遅れを数えないので、遅れの欄は出さない。 */
  readonly paced = computed(() => typeof this.speed() === 'number');
  /** 遅れの数（ランチャーが書かないときは null）。 */
  readonly lateness = computed<Lateness | null>(() => {
    const t = this.time();
    if (!t || t.lateSteps === null || t.maxLateMs === null) return null;
    return {
      lateSteps: t.lateSteps,
      maxLateMs: t.maxLateMs,
      slippedMs: t.slippedMs ?? 0,
      lateMs: t.lateMs ?? 0,
    };
  });
  /**
   * 実時間に遅れたか（許容を超えて遅れた刻みがある）。許容の内の小さな late_ms は、刻みの同期のたびに出たり消えたり
   * するので、ここには入れない（数として見せるだけ）。許容を超えて止まっている刻みは、ランチャーが late_steps に
   * その場で数える。最速に変えた後も、それまでの遅れは残る。
   */
  readonly behind = computed(() => (this.lateness()?.lateSteps ?? 0) > 0);
  /** 最後に届いた command の事象（stop も含む）。 */
  readonly lastCommand = computed<CommandView | null>(
    () => this.server.session()?.lastCommand ?? null,
  );

  /**
   * 時刻の命令を送れるか: ランチャーが対応し、シミュレーションが動いていて（段階が running で、time の事象が来ている）、
   * 結果がまだ出ておらず、止めるよう頼んでいない。サーバーと揃っていない間も送らない（古い状態を見て押さない）。
   * 段階が stopping に進んだら（stop_at_ms に着いた、participant が失敗した）、ランチャーはもう受け付けない。
   */
  readonly controllable = computed(() => {
    const session = this.server.session();
    return (
      this.supported() &&
      this.server.ready() &&
      !!session &&
      session.phase === 'running' &&
      session.launcher.running &&
      session.time !== null &&
      session.result === null &&
      session.stopRequests === 0
    );
  });

  /**
   * 送って、まだ答えを見ていない命令（HTTP の応答か、ランチャーの command の事象を待っている）。その間は、次の命令を
   * 送らない（ダブルクリックや Enter の繰り返しで、ステップ送りが 2 回にならないように）。
   */
  readonly sending = computed<TimeCommandName | null>(() => {
    const pending = this.pending();
    if (!pending) return null;
    if (this.inFlight() > 0) return pending.command;
    const session = this.server.session();
    const answered =
      !session ||
      session.id !== pending.session ||
      !session.launcher.running ||
      (session.lastCommand?.seq ?? -1) > pending.afterSeq;
    return answered ? null : pending.command;
  });
  /**
   * ステップ送りの途中（受け付けたステップで動いていて、まだ止まる時刻に着いていない）。ランチャーは、その間も
   * 命令の側では一時停止しているとみなす（再開とステップ送りを受け付ける）。
   */
  readonly stepping = computed(() => {
    const last = this.lastCommand();
    return last?.name === 'step' && last.accepted && this.paused() === false;
  });
  private readonly free = computed(() => this.controllable() && this.sending() === null);
  /** 一時停止を押せるか（動いているとき。ステップ送りの途中も、そこで止められる）。 */
  readonly canPause = computed(() => this.free() && this.paused() === false);
  /** 再開を押せるか（一時停止しているとき、またはステップ送りの途中）。 */
  readonly canResume = computed(() => this.free() && (this.paused() === true || this.stepping()));
  /** ステップ送りを押せるか（一時停止しているとき、またはステップ送りの途中。ランチャーの決まり）。 */
  readonly canStep = this.canResume;
  private readonly currentFailure = computed(() => {
    const failure = this.failure();
    return failure && failure.session === (this.server.session()?.id ?? null) ? failure : null;
  });
  /**
   * 最後の命令がうまくいかなかった理由: サーバーが断った（英語のまま）か、ランチャーが ANSWER_TIMEOUT_MS の間に答え
   * なかった（画面の文）。実行が変わったら出さない。
   */
  readonly error = computed(() => this.currentFailure()?.message ?? null);
  /** error がサーバーの文（英語）か。 */
  readonly errorFromServer = computed(() => this.currentFailure()?.kind === 'server');

  constructor() {
    inject(DestroyRef).onDestroy(() => clearTimeout(this.answerTimer));
  }

  pause(): SendResult {
    return this.send({ command: 'pause' });
  }

  resume(): SendResult {
    return this.send({ command: 'resume' });
  }

  /** 一時停止しているとき、仮想時間を ms 進めて、また止める（ランチャーが刻みの倍数に切り上げる）。 */
  step(ms: number): SendResult {
    return this.send({ command: 'step', ms });
  }

  /** 速さを変える（一時停止はそのまま）。 */
  setSpeed(value: number | 'max'): SendResult {
    return this.send({ command: 'speed', value });
  }

  /**
   * 命令を送る。結果は、ランチャーの command と time の事象で届く（応答の中身は使わない）。実行がないとき、前の命令の
   * 答えを待っている間（sending）は送らず、`{sent: false, reason}` を返す。答えが ANSWER_TIMEOUT_MS の間に来なければ、
   * 待つのをやめて error に出す。
   */
  send(command: TimeCommand): SendResult {
    const session = this.server.session();
    if (!session) return { sent: false, reason: 'no-session' };
    if (this.sending() !== null) return { sent: false, reason: 'waiting' };
    this.failure.set(null);
    const pending: Pending = {
      command: command.command,
      session: session.id,
      afterSeq: session.lastCommand?.seq ?? -1,
    };
    this.pending.set(pending);
    this.inFlight.update((n) => n + 1);
    clearTimeout(this.answerTimer);
    this.answerTimer = setTimeout(() => this.giveUp(pending), this.answerTimeoutMs);
    this.http.post('api/time', command).subscribe({
      next: () => this.inFlight.update((n) => n - 1),
      error: (error: unknown) => {
        this.inFlight.update((n) => n - 1);
        if (this.pending() !== pending) return;
        clearTimeout(this.answerTimer);
        this.pending.set(null); // 送れなかった: ランチャーの答えは来ない
        this.failure.set({
          kind: 'server',
          message: errorMessage(error),
          session: this.server.session()?.id ?? null,
        });
      },
    });
    return { sent: true };
  }

  /** 答えを待つ時間が過ぎた: まだ答えが来ていなければ、待つのをやめて、そう言う。 */
  private giveUp(pending: Pending): void {
    if (this.pending() !== pending || this.sending() === null) return;
    this.pending.set(null);
    this.failure.set({
      kind: 'no-answer',
      message: $localize`ランチャーが命令（${pending.command}:command:）に ${this.answerTimeoutMs / 1000}:seconds: 秒たっても答えません。入力を読んでいないかもしれません。もう一度送れます。`,
      session: pending.session,
    });
  }
}
