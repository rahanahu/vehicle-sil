import { Component, computed, inject } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import type { RunTarget } from '@vehicle-sil/gui-client';
import { SimulationTime } from '../core/session/simulation-time';
import { WAITING_TEXT, controlLine } from '../widgets/control-labels';
import { failureLine } from './labels';

/**
 * 一時停止と再開のボタン（実行している間、状態の帯の開始と停止の後に置く）。2 つは別のボタンで、いつもこの順に同じ
 * 場所にあり、使えない方は押せなくする（押している最中に、同じ位置のボタンの意味が入れ替わらない）。押せない間も
 * フォーカスは受け、理由（正面の口の `controls`）は各ボタンの説明（`aria-describedby`）にある。見える文は時刻のカード。
 */
@Component({
  selector: 'app-time-buttons',
  imports: [MatButtonModule],
  template: `
    @let target = time.target();
    <button
      matButton="tonal"
      type="button"
      data-testid="pause"
      [disabled]="!controls().pause.ok"
      disabledInteractive
      [attr.aria-describedby]="pauseWhy() ? 'time-pause-why' : null"
      (click)="pause(target)"
    >
      <span class="mark" aria-hidden="true">‖</span>
      <span class="text">{{ pauseText }}</span>
    </button>
    <button
      matButton="tonal"
      type="button"
      data-testid="resume"
      [disabled]="!controls().resume.ok"
      disabledInteractive
      [attr.aria-describedby]="resumeWhy() ? 'time-resume-why' : null"
      (click)="resume(target)"
    >
      <span class="mark" aria-hidden="true">▶</span>
      <span class="text">{{ resumeText }}</span>
    </button>
    <!-- ボタンの説明だけ（見た目には出さない。時刻のカードにも出る） -->
    <span
      class="why"
      id="time-pause-why"
      data-testid="pause-why"
      [attr.lang]="pauseWhy()?.english ? 'en' : null"
      >{{ pauseWhy()?.text }}</span
    >
    <span
      class="why"
      id="time-resume-why"
      data-testid="resume-why"
      [attr.lang]="resumeWhy()?.english ? 'en' : null"
      >{{ resumeWhy()?.text }}</span
    >
  `,
  styles: `
    /* ボタンは、置いた場所（状態の帯の操作の並び）にそのまま並ぶ */
    :host {
      display: contents;
    }
    .why {
      position: absolute;
      inline-size: 1px;
      block-size: 1px;
      overflow: hidden;
      clip-path: inset(50%);
      white-space: nowrap;
    }
    .mark {
      margin-inline-end: var(--app-space-1);
    }
    /* とても狭い帯では印だけを見せ、名前は読み上げにだけ残す（帯を高くしない） */
    @container run-status (inline-size < 420px) {
      button.mat-mdc-button-base {
        min-inline-size: 44px;
        padding-inline: 12px;
      }
      .mark {
        margin: 0;
      }
      .text {
        position: absolute;
        inline-size: 1px;
        block-size: 1px;
        overflow: hidden;
        clip-path: inset(50%);
        white-space: nowrap;
      }
    }
  `,
})
export class TimeButtons {
  protected readonly time = inject(SimulationTime);
  /** 押せるか、押せない理由（正面の口が言う）。 */
  protected readonly controls = this.time.controls;
  protected readonly pauseText = $localize`一時停止`;
  protected readonly resumeText = $localize`再開`;
  /**
   * ボタンの説明: 送った命令の答えを待っている間と、前の命令がうまくいかなかったとき（`status.problem`）はその文、それ以外は正面の口の
   * 理由（押せないとき）か注意（`caution`）。なければ null。
   */
  protected readonly pauseWhy = computed(() => this.why('pause'));
  protected readonly resumeWhy = computed(() => this.why('resume'));

  private why(op: 'pause' | 'resume'): { text: string; english: boolean } | null {
    if (this.time.waiting()) return { text: WAITING_TEXT, english: false };
    const failure = failureLine(this.time.failure(), this.time.answerTimeoutMs);
    if (failure) return failure;
    return controlLine(op, this.controls()[op]);
  }

  protected pause(target: RunTarget | null): void {
    if (this.controls().pause.ok) this.time.pause(target);
  }

  protected resume(target: RunTarget | null): void {
    if (this.controls().resume.ok) this.time.resume(target);
  }
}
