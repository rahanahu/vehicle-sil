import { HttpTransport, errorMessage } from './http';
import { AppInfo, TimeCommand, TimeCommandName } from './protocol';
import { CommandView, SessionView, TimeView } from './session-state';
import { ErrorSink, Readable, Writable, derived, rethrowLater, shallowEqual } from './values';

/** ランチャーの features のうち、時刻にかかわるもの。 */
export const FEATURE_SPEED = 'speed';
export const FEATURE_START_PAUSED = 'start-paused';
export const FEATURE_TIME_COMMANDS = 'time-commands';

/**
 * 送った命令へのランチャーの答え（command の事象）を待つ時間（ミリ秒）。過ぎたら待つのをやめて、操作を押せるように
 * 戻し、答えが来ないことを誤りとして出す（入力を読まないランチャー）。
 */
export const ANSWER_TIMEOUT_MS = 5000;

/** 送らなかった理由: 実行がない、前の命令の答えを待っている。 */
export type NotSentReason = 'no-session' | 'waiting';

/** send() などの結果: 送ったか。送らなかったなら、その理由。 */
export type SendResult = { sent: true } | { sent: false; reason: NotSentReason };

/** 時刻の命令がうまくいかなかったこと。 */
export interface TimeFailure {
  /** server: サーバーが断った（message はサーバーの英語の文）、no-answer: ランチャーが答えなかった。 */
  kind: 'server' | 'no-answer';
  /** 人に見せる英語の文。画面の言葉にするときは kind と command、answerTimeoutMs から作る。 */
  message: string;
  command: TimeCommandName;
  /** 送った実行の番号。 */
  session: number | null;
}

/** 遅れの数（等速などで速さを合わせているときだけ意味がある）。 */
export interface Lateness {
  lateSteps: number;
  maxLateMs: number;
  slippedMs: number;
  lateMs: number;
}

/** 時刻の状態と、いま押してよい命令。値はどれも、セッションの写しとサーバーの情報から作る。 */
export interface TimeState {
  /** ランチャーが時刻の命令（pause、resume、step、speed）を持つか。持たなければ操作を出さない。 */
  supported: boolean;
  /** いまのセッションの時刻（time の事象が来るまでは null）。 */
  time: TimeView | null;
  /** 速さを合わせている（数の速さ）。最速では遅れを数えない。 */
  paced: boolean;
  /** 遅れの数（ランチャーが書かないときは null）。 */
  lateness: Lateness | null;
  /**
   * 実時間に遅れたか（許容を超えて遅れた刻みがある）。許容の内の小さな late_ms は、刻みの同期のたびに出たり消えたり
   * するので、ここには入れない。最速に変えた後も、それまでの遅れは残る。
   */
  behind: boolean;
  /** 最後に届いた command の事象（stop も含む）。 */
  lastCommand: CommandView | null;
  /**
   * 時刻の命令を送れるか: ランチャーが対応し、セッションの写しが揃っていて、段階が running で、time の事象が来ていて、
   * 結果がまだで、止めるよう頼んでいない。段階が stopping に進んだら、ランチャーはもう受け付けない。
   */
  controllable: boolean;
  /**
   * 送って、まだ答えを見ていない命令（HTTP の応答か、ランチャーの command の事象を待っている）。その間は、次の命令を
   * 送らない（ダブルクリックや Enter の繰り返しで、ステップ送りが 2 回にならないように）。
   */
  sending: TimeCommandName | null;
  /** ステップ送りの途中（ランチャーは、その間も一時停止の扱いで、再開とステップ送りを受け付ける）。 */
  stepping: boolean;
  /** 一時停止を押せるか（動いているとき。ステップ送りの途中も）。 */
  canPause: boolean;
  /** 再開を押せるか（一時停止しているとき、またはステップ送りの途中）。 */
  canResume: boolean;
  /** ステップ送りを押せるか（canResume と同じ。ランチャーの決まり）。 */
  canStep: boolean;
  /** 最後の命令がうまくいかなかったこと。実行が変わったら null。 */
  failure: TimeFailure | null;
}

/** 送って、ランチャーの答え（command の事象）をまだ見ていない命令。 */
interface Pending {
  command: TimeCommandName;
  session: number;
  /** 送ったときの最後の command の事象の seq。これより後の command の事象が答え。 */
  afterSeq: number;
}

/** 時刻の、読むための口と命令（GuiClient.time）。 */
export interface ClientTime {
  /** 時刻の状態と、いま押してよい命令。 */
  readonly state: Readable<TimeState>;
  /** 命令の答えを待つ時間（ミリ秒）。 */
  readonly answerTimeoutMs: number;
  pause(): SendResult;
  resume(): SendResult;
  /** 一時停止しているとき、仮想時間を ms 進めて、また止める（ランチャーが刻みの倍数に切り上げる）。 */
  step(ms: number): SendResult;
  /** 速さを変える（一時停止はそのまま）。 */
  setSpeed(value: number | 'max'): SendResult;
  /**
   * 命令を送る。結果は、ランチャーの command と time の事象で届く。実行がないとき、前の命令の答えを待っている間は
   * 送らず、`{sent: false, reason}` を返す。押せるかは、送る前に state の canPause などで確かめる。
   */
  send(command: TimeCommand): SendResult;
}

/**
 * シミュレーションの時刻: 状態（TimeState）を読める値で出し、時刻の命令（一時停止、再開、ステップ送り、速さの変更）
 * を GUI サーバーへ送る（`POST api/time`）。
 *
 * - 状態はセッションの写し（要約とランチャーの事象。要約が正）から作る。途中からつないでも、要約で同じ状態が出る。
 * - 命令はサーバーがランチャーに渡して、すぐ返る。受け付けたかは lastCommand、効いたことは time に出る。
 */
export class TimeControl implements ClientTime {
  private readonly inFlight: Writable<number>;
  private readonly pending: Writable<Pending | null>;
  private readonly failureValue: Writable<TimeFailure | null>;
  private answerTimer: ReturnType<typeof setTimeout> | undefined;
  private lastLateness: Lateness | null = null;

  /** 時刻の状態と、いま押してよい命令。 */
  readonly state: Readable<TimeState>;

  constructor(
    private readonly session: Readable<SessionView | null>,
    ready: Readable<boolean>,
    app: Readable<AppInfo | null>,
    private readonly transport: HttpTransport,
    /** 答えを待つ時間（ミリ秒）。 */
    readonly answerTimeoutMs = ANSWER_TIMEOUT_MS,
    onError: ErrorSink = rethrowLater,
  ) {
    this.inFlight = new Writable<number>(0, Object.is, onError);
    this.pending = new Writable<Pending | null>(null, Object.is, onError);
    this.failureValue = new Writable<TimeFailure | null>(null, Object.is, onError);
    this.state = derived(
      [session, ready, app, this.inFlight, this.pending, this.failureValue],
      () => this.compute(session.get(), ready.get(), app.get()),
      shallowEqual,
      onError,
    );
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
   * 答えを待っている間（sending）は送らず、`{sent: false, reason}` を返す。答えが answerTimeoutMs の間に来なければ、
   * 待つのをやめて failure に出す。押せるかは、送る前に canPause などで確かめる。
   */
  send(command: TimeCommand): SendResult {
    const session = this.session.get();
    if (!session) return { sent: false, reason: 'no-session' };
    if (this.state.get().sending !== null) return { sent: false, reason: 'waiting' };
    this.failureValue.set(null);
    const pending: Pending = {
      command: command.command,
      session: session.id,
      afterSeq: session.lastCommand?.seq ?? -1,
    };
    this.pending.set(pending);
    this.inFlight.update((n) => n + 1);
    clearTimeout(this.answerTimer);
    this.answerTimer = setTimeout(() => this.giveUp(pending), this.answerTimeoutMs);
    this.transport({ method: 'POST', path: 'api/time', body: command }, (outcome) => {
      this.inFlight.update((n) => n - 1);
      if (outcome.ok || this.pending.get() !== pending) return;
      clearTimeout(this.answerTimer);
      this.pending.set(null); // 送れなかった: ランチャーの答えは来ない
      this.failureValue.set({
        kind: 'server',
        message: errorMessage(outcome.error),
        command: pending.command,
        session: this.session.get()?.id ?? null,
      });
    });
    return { sent: true };
  }

  /** 待ちの時計を止め、答えを待つのをやめる（クライアントを閉じるとき）。 */
  dispose(): void {
    clearTimeout(this.answerTimer);
    this.pending.set(null);
  }

  /** 答えを待つ時間が過ぎた: まだ答えが来ていなければ、待つのをやめて、そう言う。 */
  private giveUp(pending: Pending): void {
    if (this.pending.get() !== pending || this.state.get().sending === null) return;
    this.pending.set(null);
    this.failureValue.set({
      kind: 'no-answer',
      message:
        `the launcher did not answer the command (${pending.command}) within ` +
        `${this.answerTimeoutMs / 1000} s; it may not read its input. You can send it again.`,
      command: pending.command,
      session: pending.session,
    });
  }

  private compute(session: SessionView | null, ready: boolean, app: AppInfo | null): TimeState {
    const supported = app?.launcher.features.includes(FEATURE_TIME_COMMANDS) ?? false;
    const time = session?.time ?? null;
    let lateness: Lateness | null =
      !time || time.lateSteps === null || time.maxLateMs === null
        ? null
        : {
            lateSteps: time.lateSteps,
            maxLateMs: time.maxLateMs,
            slippedMs: time.slippedMs ?? 0,
            lateMs: time.lateMs ?? 0,
          };
    // 同じ数なら前のものを使う（ほかの欄だけが変わったときに、遅れが変わったと知らせない）
    if (lateness && this.lastLateness && shallowEqual(lateness, this.lastLateness)) {
      lateness = this.lastLateness;
    }
    this.lastLateness = lateness;
    const lastCommand = session?.lastCommand ?? null;
    const controllable =
      supported &&
      ready &&
      !!session &&
      session.phase === 'running' &&
      session.launcher.running &&
      session.time !== null &&
      session.result === null &&
      session.stopRequests === 0;
    const pending = this.pending.get();
    let sending: TimeCommandName | null = null;
    if (pending) {
      const answered =
        !session ||
        session.id !== pending.session ||
        !session.launcher.running ||
        (session.lastCommand?.seq ?? -1) > pending.afterSeq;
      sending = this.inFlight.get() > 0 || !answered ? pending.command : null;
    }
    const paused = time?.paused ?? null;
    const stepping = lastCommand?.name === 'step' && lastCommand.accepted && paused === false;
    const free = controllable && sending === null;
    const canResume = free && (paused === true || stepping);
    const failure = this.failureValue.get();
    return {
      supported,
      time,
      paced: typeof time?.speed === 'number',
      lateness,
      behind: (lateness?.lateSteps ?? 0) > 0,
      lastCommand,
      controllable,
      sending,
      stepping,
      canPause: free && paused === false,
      canResume,
      canStep: canResume,
      failure: failure && failure.session === (session?.id ?? null) ? failure : null,
    };
  }
}
