import { Component, computed, inject } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { SimulationTime } from '../core/session/simulation-time';
import { WAITING_TEXT } from './labels';

/**
 * 一時停止と再開のボタン（実行している間、状態の帯の開始と停止の後に置く）。2 つは別のボタンで、いつもこの順に同じ
 * 場所にあり、使えない方は押せなくする（押している最中に、同じ位置のボタンの意味が入れ替わらない）。押せない間も
 * フォーカスは受け、理由は帯の文が言う（送った命令の答えを待っている間と、答えが来なかったときは、その文）。
 */
@Component({
  selector: 'app-time-buttons',
  imports: [MatButtonModule],
  template: `
    <button
      matButton="tonal"
      type="button"
      data-testid="pause"
      [disabled]="!time.canPause()"
      disabledInteractive
      [attr.aria-describedby]="why() ? 'time-buttons-why' : time.canPause() ? null : 'run-outcome'"
      (click)="pause()"
    >
      <span class="mark" aria-hidden="true">‖</span>
      <span class="text">{{ pauseText }}</span>
    </button>
    <button
      matButton="tonal"
      type="button"
      data-testid="resume"
      [disabled]="!time.canResume()"
      disabledInteractive
      [attr.aria-describedby]="why() ? 'time-buttons-why' : time.canResume() ? null : 'run-outcome'"
      (click)="resume()"
    >
      <span class="mark" aria-hidden="true">▶</span>
      <span class="text">{{ resumeText }}</span>
    </button>
    <!-- ボタンの説明だけ（見た目には出さない。カードにも同じことが出る） -->
    <span class="why" id="time-buttons-why" data-testid="time-buttons-why">{{ why() }}</span>
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
  protected readonly pauseText = $localize`一時停止`;
  protected readonly resumeText = $localize`再開`;
  /** 押せない（または前の命令がうまくいかなかった）理由のうち、帯の文にないもの。なければ ''。 */
  protected readonly why = computed(() => {
    if (this.time.controllable() && this.time.sending() !== null) return WAITING_TEXT;
    return this.time.error() ?? '';
  });

  protected pause(): void {
    if (this.time.canPause()) this.time.pause();
  }

  protected resume(): void {
    if (this.time.canResume()) this.time.resume();
  }
}
