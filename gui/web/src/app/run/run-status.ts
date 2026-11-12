import {
  Component,
  DOCUMENT,
  DestroyRef,
  ElementRef,
  InjectionToken,
  LOCALE_ID,
  afterRenderEffect,
  computed,
  effect,
  inject,
  input,
  output,
  viewChild,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatDialog, MatDialogRef } from '@angular/material/dialog';
import { PHASES, RunOutcome, SessionView, phaseIndex, severityRank } from '@vehicle-sil/gui-client';
import { MeasurePipe } from '../core/format/measure-pipe';
import { settledAfter } from '../core/streams/stream-signals';
import { ForceStopDialog } from './force-stop-dialog';
import { LATE_ANNOUNCEMENT, latenessText, phaseLabel, runOutcomeLabel, speedLabel } from './labels';
import { StopAction } from './run-session';

/**
 * 操作のボタンが押せるようになってから、押しても何もしない時間（ミリ秒）。ダブルクリックの 2 回目や Enter の
 * 連打が、押せるようになったばかりの次の操作に届かないように（キーボードの Enter と Space も同じ）。
 */
export const ACTION_GUARD_MS = 800;

/** ACTION_GUARD_MS を差し替える口（単体試験が短くする）。 */
export const ACTION_GUARD = new InjectionToken<number>('ACTION_GUARD', {
  factory: () => ACTION_GUARD_MS,
});

/**
 * 状態の帯: いつも見えるところに、操作（開始 / 停止 / 待たずに止める）、結果、段階、経過と仮想時刻。
 * 画面の上に貼り付き、スクロールしても残る。狭い幅では折り返し、段階はいまの段階だけを出す。
 *
 * 開始と停止は別のボタンで、帯の決まった位置にいつも置く（押している最中に意味が入れ替わらない）。使えない方は
 * 押せなくする（フォーカスは受ける）。押せるようになったばかりのボタンは、ACTION_GUARD_MS の間は押しても何もしない。
 *
 * 帯の操作には、外から操作を足せる（`<app-run-status><app-time-buttons runStatusActions /></app-run-status>` のように、
 * `runStatusActions` の属性を付けた要素を中に置く）。開始と停止の後、「待たずに止める」の前に並ぶので、足した操作が
 * 出ても消えても、開始と停止は動かない。
 */
@Component({
  selector: 'app-run-status',
  imports: [MatButtonModule, MeasurePipe],
  templateUrl: './run-status.html',
  styleUrl: './run-status.css',
})
export class RunStatus {
  private readonly list = new Intl.ListFormat(inject(LOCALE_ID), { type: 'conjunction' });
  private readonly dialog = inject(MatDialog);
  private readonly document = inject(DOCUMENT);
  private readonly guardMs = inject(ACTION_GUARD);
  private forceDialog: MatDialogRef<ForceStopDialog, boolean> | null = null;

  readonly session = input.required<SessionView | null>();
  readonly outcome = input.required<RunOutcome | null>();
  /** サーバーと揃っている（つながっていて、再送を受け終えた）。揃っていなければ開始しない。 */
  readonly ready = input.required<boolean>();
  /** 開始を送って、応答を待っている。 */
  readonly starting = input(false);
  /** 停止を送って、応答を待っている。 */
  readonly stopping = input<StopAction | null>(null);
  /** 開始の応答を待っている間に、止めるよう頼んだ。 */
  readonly stopWhileStarting = input(false);
  /** HTTP でサーバーに届く（届かないとわかっていたら偽）。停止は、WebSocket が切れていても HTTP で頼める。 */
  readonly reachable = input(true);
  readonly elapsedMs = input.required<number | null>();
  /** 始められない理由（なければ空）。 */
  readonly blockedBy = input.required<readonly string[]>();
  /**
   * 前の実行と違う構成ファイルを選んでいる（ランチャーは止まっている）。開始のボタンは「開始」にし、出ている結果は
   * 前の実行のものだと添える。
   */
  readonly otherFile = input(false);

  readonly start = output();
  readonly stop = output();
  readonly forceStop = output();
  /** 結果（または異常終了）の詳しい表示へ移りたい。 */
  readonly showDetails = output();

  private readonly outcomeElement = viewChild.required<ElementRef<HTMLElement>>('outcome');
  /** 待たずに止めるボタンが、最後にフォーカスを持っていた（消えたら、帯の文へフォーカスを移す）。 */
  private forceHadFocus = false;

  private readonly running = computed(() => this.session()?.launcher.running ?? false);
  private readonly stopRequests = computed(() => this.session()?.stopRequests ?? 0);

  /** 開始を押せない理由（押せるなら null）。 */
  protected readonly startBlocked = computed<string | null>(() => {
    if (this.running()) return $localize`実行中は開始できません。`;
    if (this.starting()) return $localize`開始の応答を待っています。`;
    if (this.stopping() !== null) return $localize`停止の応答を待っています。`;
    if (!this.ready()) return $localize`サーバーと揃うまで、開始できません。`;
    return this.blockedBy()[0] ?? null;
  });

  /**
   * 止めている最中（頼んだ、頼んでいる、開始の応答の後に届ける）。停止のボタンは「止めています…」。実行が終わったら
   * （止める実行がない）、止めている最中ではない。
   */
  protected readonly stopInProgress = computed(
    () =>
      (this.running() || this.starting()) &&
      (this.stopRequests() >= 1 || this.stopping() !== null || this.stopWhileStarting()),
  );

  /**
   * 停止が意味を持つか: ランチャーが動いている（と最後に知った）か、開始の応答を待っている。WebSocket が切れていて
   * 最新の状態が見えなくても、HTTP で頼める（順に止める停止は何度頼んでも同じ）。
   */
  protected readonly stopApplies = computed(() => this.running() || this.starting());

  /** 停止を押せない理由（押せるなら null）。止めている最中は、ボタンの文が言う。 */
  protected readonly stopBlocked = computed<string | null>(() => {
    if (!this.stopApplies()) return $localize`止める実行はありません。`;
    if (this.stopInProgress()) return $localize`止めるよう頼みました。`;
    if (!this.reachable()) return $localize`サーバーに届かないので、止めるよう頼めません。`;
    return null;
  });

  /** 「待たずに止める」を出すか: 順に止めるよう頼んだ後、まだ待たずに止めるよう頼んでいないとき。 */
  protected readonly canForce = computed(() => this.running() && this.stopRequests() === 1);
  /** 「待たずに止める」を押せるか（最新の状態が見えているときだけ）。 */
  private readonly forceEnabled = computed(
    () => this.canForce() && this.ready() && this.stopping() === null,
  );

  protected readonly startArmed = settledAfter(
    computed(() => this.startBlocked() === null),
    this.guardMs,
  );
  protected readonly stopArmed = settledAfter(
    computed(() => this.stopBlocked() === null),
    this.guardMs,
  );
  protected readonly forceArmed = settledAfter(this.forceEnabled, this.guardMs);

  /** 帯に出す理由の 1 行: 開始が関係する間は開始できない理由、止められる間は停止の事情。 */
  protected readonly reason = computed<string | null>(() => {
    if (this.stopApplies()) {
      if (!this.reachable()) return this.stopBlocked();
      if (!this.ready() && !this.stopInProgress()) {
        return $localize`最新の状態は見えていませんが、止めるよう頼むことはできます。`;
      }
      return null;
    }
    return this.startBlocked();
  });

  protected readonly elapsedSeconds = computed(() => {
    const ms = this.elapsedMs();
    return ms === null ? null : ms / 1000;
  });
  /**
   * 仮想時刻。古いランチャー（time の事象に paused がない）は、動き始めた 0 と stop_at_ms に達したときしか知らせない
   * ので、動いている間の 0 は出さない（止まって見えるため）。新しいランチャーは動いている間も知らせるので、0 も出す。
   */
  protected readonly simSeconds = computed(() => {
    const session = this.session();
    const simMs = session?.simMs ?? null;
    const moves = session?.time?.paused !== null && session?.time?.paused !== undefined;
    if (simMs === null || (simMs === 0 && session?.launcher.running && !moves)) return null;
    return simMs / 1000;
  });
  /** いまの速度（ランチャーが書くとき。実行中だけ）。 */
  protected readonly speedText = computed(() => {
    const session = this.session();
    const speed = session?.launcher.running ? (session.time?.speed ?? null) : null;
    return speed === null ? null : speedLabel(speed);
  });
  /** 一時停止している（実行中で、止めるよう頼んでいないとき）。 */
  private readonly paused = computed(
    () => this.outcome() === 'running' && this.session()?.time?.paused === true,
  );
  /**
   * 実時間への遅れ（実行中で、許容を超えて遅れた刻みがあるとき。色だけでなく文で）。途中で最速に変えても、それまでの
   * 遅れは出したまま。結果が出たら、結果のカードが言う。
   */
  protected readonly lateText = computed(() => {
    const session = this.session();
    const time = session?.time;
    if (!session?.launcher.running || session.result || !time || time.lateSteps === null)
      return null;
    return latenessText({
      lateSteps: time.lateSteps,
      maxLateMs: time.maxLateMs ?? 0,
      slippedMs: time.slippedMs ?? 0,
    });
  });
  /**
   * 読み上げる遅れの知らせ。遅れが「ある」になったときだけ変わる（数が変わるたびには読み上げない）。読み上げる領域は
   * 画面でここだけ。
   */
  protected readonly lateAnnouncement = computed(() =>
    this.lateText() === null ? '' : LATE_ANNOUNCEMENT,
  );
  /** 仮想時刻の事象で、要約に入りきらずサーバーが省いた欄（あれば、その説明）。 */
  protected readonly timeOmitted = computed(() => {
    const fields = this.session()?.timeOmittedFields ?? [];
    return fields.length === 0
      ? null
      : $localize`仮想時刻の事象の欄 ${this.list.format(fields)}:fields: は、大きすぎてサーバーが省きました`;
  });

  /** 読めなかった部分の名前（あれば、並べた文）。 */
  protected readonly unreadable = computed(() => {
    const parts = this.session()?.unreadable ?? [];
    return parts.length === 0 ? null : this.list.format(parts);
  });

  /** 結果の印と色: 成功は ✓、利用者が止めた中断は ■（失敗の色にしない）、ほかは ✕。 */
  private resultTone(result: string | undefined): { tone: string; mark: string } {
    if (result === 'passed') return { tone: 'good', mark: '✓' };
    if (result === 'interrupted') return { tone: 'neutral', mark: '■' };
    return { tone: 'bad', mark: '✕' };
  }

  /** 結果の 1 行（色だけで伝えないよう、印と文で）。止めている間は、ボタンが「止めています…」なので、待っているものだけ。 */
  protected readonly summary = computed(() => {
    if (this.starting()) {
      return {
        tone: 'active',
        mark: '●',
        text: this.stopWhileStarting()
          ? $localize`開始をやめます（ランチャーが起動したところで止めます）`
          : $localize`始めています…`,
      };
    }
    const session = this.session();
    const outcome = this.outcome();
    if (!session || !outcome)
      return { tone: 'plain', mark: '', text: $localize`まだ実行していません。` };
    const result = session.result;
    switch (outcome) {
      case 'running':
        return this.paused()
          ? { tone: 'active', mark: '‖', text: $localize`一時停止中` }
          : { tone: 'active', mark: '●', text: $localize`実行中` };
      case 'stopping':
        return {
          tone: 'active',
          mark: '●',
          text: $localize`participant が終わるのを待っています`,
        };
      case 'forcing':
        return {
          tone: 'active',
          mark: '●',
          text: $localize`残っている participant を強制終了するのを待っています`,
        };
      case 'finishing':
        return {
          ...this.resultTone(result?.result),
          text: $localize`${runOutcomeLabel(result?.result ?? '')}:outcome:（ランチャーが終わるのを待っています）`,
        };
      case 'ended':
        return {
          ...this.resultTone(result?.result),
          text: this.otherFile()
            ? $localize`前の実行: ${runOutcomeLabel(result?.result ?? '')}:outcome:`
            : runOutcomeLabel(result?.result ?? ''),
        };
      case 'crashed': {
        const { exitCode, signal } = session.launcher;
        const how =
          exitCode !== null
            ? $localize`終了コード ${exitCode}:code:`
            : signal !== null
              ? $localize`シグナル ${signal}:signal:`
              : $localize`終わり方は不明`;
        const text = $localize`異常終了（${how}:how:）`;
        return {
          tone: 'bad',
          mark: '✕',
          text: this.otherFile() ? $localize`前の実行: ${text}:outcome:` : text,
        };
      }
    }
  });

  /** 失敗したとき、最初の誤り（ランチャーの英語の文）。 */
  protected readonly firstError = computed(() => {
    const session = this.session();
    // 利用者が止めた中断は誤りではない（所見は注記の interrupted だけ）
    if (!session?.result || ['passed', 'interrupted'].includes(session.result.result)) return null;
    const findings = session.result.findings;
    const worst = [...findings].sort(
      (a, b) => severityRank(a.severity) - severityRank(b.severity),
    )[0];
    return worst ? `${worst.code}: ${worst.message}` : null;
  });

  /** 動いているランチャーから、心拍も含めて何も届かない秒数（なければ null）。 */
  protected readonly silentSeconds = computed(() => {
    const session = this.session();
    return session?.launcher.running ? session.launcher.silentSeconds : null;
  });

  protected readonly hasDetails = computed(() => {
    const outcome = this.outcome();
    return outcome === 'ended' || outcome === 'crashed' || outcome === 'finishing';
  });

  /** 段階。結果を出さずに終わったら、止まった段階に印を付け、「終了」まで進める。 */
  protected readonly steps = computed(() => {
    const session = this.session();
    const crashed = this.outcome() === 'crashed';
    const reached = session ? session.phaseRank : -1;
    const current = session && !session.launcher.running ? phaseIndex('ended') : reached;
    return PHASES.map((phase, i) => {
      const cut = crashed && i === reached && phase !== 'ended';
      return {
        phase,
        label: cut ? $localize`${phaseLabel(phase)}:phase:（中断）` : phaseLabel(phase),
        // 中断した段階より後（終了の前）は通っていない
        state: cut
          ? 'cut'
          : crashed && i > reached && i < current
            ? 'todo'
            : i < current
              ? 'done'
              : i === current
                ? 'current'
                : 'todo',
      };
    });
  });
  protected readonly unknownPhase = computed(() => {
    const phase = this.session()?.phase ?? null;
    return phase !== null && phaseIndex(phase) < 0 ? phase : null;
  });

  protected readonly startText = computed(() =>
    this.session() && !this.otherFile() ? $localize`もう一度実行` : $localize`開始`,
  );
  protected readonly stopText = computed(() =>
    this.stopInProgress() ? $localize`止めています…` : $localize`停止`,
  );

  constructor() {
    // 確かめのダイアログを開いている間に「待たずに止める」が意味を失ったら（実行が終わったなど）、閉じる
    effect(() => {
      if (!this.canForce()) this.forceDialog?.close(false);
    });
    // 「待たずに止める」がフォーカスを持ったまま消えたら、いまの状態を言う帯の文へフォーカスを移す（DOM の操作）
    afterRenderEffect(() => {
      if (this.canForce() || !this.forceHadFocus) return;
      this.forceHadFocus = false;
      const active = this.document.activeElement;
      if (!active || active === this.document.body) this.outcomeElement().nativeElement.focus();
    });
    inject(DestroyRef).onDestroy(() => this.forceDialog?.close(false));
  }

  protected pressStart(): void {
    if (this.startBlocked() === null && this.startArmed()) this.start.emit();
  }

  protected pressStop(): void {
    if (this.stopBlocked() === null && this.stopArmed()) this.stop.emit();
  }

  protected forceFocus(focused: boolean, event?: FocusEvent): void {
    // フォーカスが別の要素へ移ったときだけ忘れる（要素が消えたときの blur は、移り先がない）
    if (focused) this.forceHadFocus = true;
    else if (event?.relatedTarget) this.forceHadFocus = false;
  }

  protected askForceStop(): void {
    if (!this.forceEnabled() || !this.forceArmed() || this.forceDialog) return;
    this.forceDialog = this.dialog.open<ForceStopDialog, void, boolean>(ForceStopDialog);
    this.forceDialog.afterClosed().subscribe((confirmed) => {
      this.forceDialog = null;
      if (confirmed && this.forceEnabled()) this.forceStop.emit();
      // ボタンが消えていれば（ダイアログの間に実行が終わった）、帯の文へ
      if (!this.canForce()) this.outcomeElement().nativeElement.focus();
    });
  }
}
