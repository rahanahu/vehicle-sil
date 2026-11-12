import { Component, LOCALE_ID, computed, inject, input, output } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatDialog } from '@angular/material/dialog';
import { PHASES, severityRank } from '../core/api';
import { MeasurePipe } from '../core/format/measure-pipe';
import { RunOutcome, SessionView, phaseIndex } from '../core/session/session-state';
import { ForceStopDialog } from './force-stop-dialog';
import { phaseLabel, runOutcomeLabel } from './labels';
import { RunAction } from './run-session';

/** 主なボタンの役目。ボタンは 1 つの要素のまま、文言と動きだけを変える（押した後もフォーカスが残る）。 */
type Primary = 'start' | 'again' | 'stop' | 'stopping';

/**
 * 状態の帯: いつも見えるところに、主な操作（開始 / 停止 / 待たずに止める / もう一度）、結果、段階、経過と仮想時刻。
 * 画面の上に貼り付き、スクロールしても残る。狭い幅では折り返し、段階はいまの段階だけを出す。
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

  readonly session = input.required<SessionView | null>();
  readonly outcome = input.required<RunOutcome | null>();
  /** サーバーと揃っている（つながっていて、再送を受け終えた）。揃っていなければ開始も停止もしない。 */
  readonly ready = input.required<boolean>();
  readonly pending = input.required<RunAction | null>();
  readonly elapsedMs = input.required<number | null>();
  /** 始められない理由（なければ空）。 */
  readonly blockedBy = input.required<readonly string[]>();
  /**
   * 前の実行と違う構成ファイルを選んでいる（ランチャーは止まっている）。主なボタンは「開始」にし、出ている結果は
   * 前の実行のものだと添える。
   */
  readonly otherFile = input(false);

  readonly start = output();
  readonly stop = output();
  readonly forceStop = output();
  /** 結果（または異常終了）の詳しい表示へ移りたい。 */
  readonly showDetails = output();

  private readonly running = computed(() => this.session()?.launcher.running ?? false);
  private readonly stopRequests = computed(() => this.session()?.stopRequests ?? 0);

  protected readonly primary = computed<Primary>(() => {
    if (this.running()) return this.stopRequests() >= 1 ? 'stopping' : 'stop';
    return this.session() && !this.otherFile() ? 'again' : 'start';
  });

  /** 主なボタンを押せない理由（押せるなら null）。 */
  protected readonly primaryBlocked = computed<string | null>(() => {
    if (!this.ready()) return $localize`サーバーと揃うまで、開始も停止もできません。`;
    if (this.pending() !== null) return $localize`サーバーの応答を待っています。`;
    switch (this.primary()) {
      case 'stopping':
        return null;
      case 'stop':
        return null;
      default:
        return this.blockedBy()[0] ?? null;
    }
  });

  protected readonly primaryDisabled = computed(
    () => this.primary() === 'stopping' || this.primaryBlocked() !== null,
  );

  /** 「待たずに止める」を出すか: 順に止めるよう頼んだ後、まだ待たずに止めるよう頼んでいないとき。 */
  protected readonly canForce = computed(() => this.running() && this.stopRequests() === 1);

  protected readonly elapsedSeconds = computed(() => {
    const ms = this.elapsedMs();
    return ms === null ? null : ms / 1000;
  });
  /**
   * 仮想時刻。いまのランチャーは、動き始めた 0 と stop_at_ms に達したときしか知らせないので、動いている間の 0 は
   * 出さない（止まって見えるため）。進んだ値と、終わった後の値を出す。
   */
  protected readonly simSeconds = computed(() => {
    const session = this.session();
    const simMs = session?.simMs ?? null;
    if (simMs === null || (simMs === 0 && session?.launcher.running)) return null;
    return simMs / 1000;
  });
  /** 仮想時刻の事象で、要約に入りきらずサーバーが省いた欄（あれば、その説明）。 */
  protected readonly timeOmitted = computed(() => {
    const fields = this.session()?.timeOmittedFields ?? [];
    return fields.length === 0
      ? null
      : $localize`仮想時刻の事象の欄 ${this.list.format(fields)}:fields: は、大きすぎてサーバーが省きました`;
  });

  /** 結果の印と色: 成功は ✓、利用者が止めた中断は ■（失敗の色にしない）、ほかは ✕。 */
  private resultTone(result: string | undefined): { tone: string; mark: string } {
    if (result === 'passed') return { tone: 'good', mark: '✓' };
    if (result === 'interrupted') return { tone: 'neutral', mark: '■' };
    return { tone: 'bad', mark: '✕' };
  }

  /** 結果の 1 行（色だけで伝えないよう、印と文で）。止めている間は、ボタンが「止めています…」なので、待っているものだけ。 */
  protected readonly summary = computed(() => {
    const session = this.session();
    const outcome = this.outcome();
    if (!session || !outcome)
      return { tone: 'plain', mark: '', text: $localize`まだ実行していません。` };
    const result = session.result;
    switch (outcome) {
      case 'running':
        return { tone: 'active', mark: '●', text: $localize`実行中` };
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

  protected readonly primaryText = computed(() => {
    switch (this.primary()) {
      case 'start':
        return $localize`開始`;
      case 'again':
        return $localize`もう一度実行`;
      case 'stop':
        return $localize`停止`;
      case 'stopping':
        return $localize`止めています…`;
    }
  });

  protected press(): void {
    if (this.primaryDisabled()) return;
    if (this.primary() === 'stop') this.stop.emit();
    else this.start.emit();
  }

  protected askForceStop(): void {
    this.dialog
      .open<ForceStopDialog, void, boolean>(ForceStopDialog)
      .afterClosed()
      .subscribe((confirmed) => {
        if (confirmed && this.canForce()) this.forceStop.emit();
      });
  }
}
