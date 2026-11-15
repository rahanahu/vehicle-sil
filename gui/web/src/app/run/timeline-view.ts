import { CUSTOM_ELEMENTS_SCHEMA, Component, computed, inject, input, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import type { VehicleSilTimelineEventMap } from '@vehicle-sil/gui-client/timeline';
import { AuthToken } from '../core/auth/auth-token';
import { LanguageChoice } from '../core/i18n/language-choice';

/** 要素 <vehicle-sil-timeline> を定義するスクリプトを、初めて表示するときに 1 度だけ読む（初めの読み込みを重くしない）。 */
let defining: Promise<unknown> | null = null;
const defineTimeline = (): Promise<unknown> =>
  (defining ??= import('@vehicle-sil/gui-client/timeline'));

/**
 * 実行の時間の軸を、この画面の中に要素 <vehicle-sil-timeline> で出す（開閉できる）。利用者の画面に要素を置く手本:
 * CUSTOM_ELEMENTS_SCHEMA、プロパティは signal を [prop] で、事象は (event) で受ける（docs/gui.md の
 * 「自分の画面にタイムラインを置く」）。カーソルは、要素の事象で signal に入れ、[cursor] で要素に戻す。
 */
@Component({
  selector: 'app-timeline-view',
  imports: [MatButtonModule],
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    <button
      mat-stroked-button
      type="button"
      data-testid="timeline-toggle"
      [attr.aria-expanded]="open()"
      (click)="toggle()"
    >
      @if (open()) {
        <ng-container i18n>時間の軸を閉じる</ng-container>
      } @else {
        <ng-container i18n>時間の軸をここに表示</ng-container>
      }
    </button>
    @if (open() && defined()) {
      <vehicle-sil-timeline
        data-testid="timeline-element"
        [src]="src()"
        [lang]="language"
        [token]="token()"
        [compact]="true"
        [hide]="hidden"
        [cursor]="cursor()"
        (cursorchange)="onCursor($event)"
        (stepselect)="onStep($event)"
      />
      <p class="readout" data-testid="timeline-readout">
        @if (cursor() !== null) {
          <span i18n>カーソル: {{ cursor()?.toFixed(1) }} ms</span>
        }
        @if (step() !== null) {
          <span i18n>選んだ手順: {{ step() }}</span>
        }
      </p>
    }
  `,
  styles: `
    :host {
      display: block;
      margin: 0 0 var(--app-space-2);
    }
    vehicle-sil-timeline {
      margin-block-start: var(--app-space-2);
      max-block-size: 70vh;
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: 6px;
    }
    .readout {
      display: flex;
      gap: var(--app-space-2);
      margin: var(--app-space-1) 0 0;
      color: var(--mat-sys-on-surface-variant);
    }
  `,
})
export class TimelineView {
  protected readonly language = inject(LanguageChoice).current;
  protected readonly token = inject(AuthToken).token;

  /** サーバーの、実行の時間の軸のファイル（`api/timeline/<id>`。サーバーの根からの相対）。 */
  readonly timelineUrl = input.required<string>();

  /** 要素の中で出さない部分（カードの中は狭いので、操作の説明を省く）。 */
  protected readonly hidden = 'help';
  protected readonly src = computed(() => `/${this.timelineUrl()}`);
  protected readonly open = signal(false);
  protected readonly defined = signal(false);
  /** 要素のカーソル（仮想時刻の ms）と選んだ手順。要素の事象で入り、[cursor] で要素に戻る。 */
  protected readonly cursor = signal<number | null>(null);
  protected readonly step = signal<number | null>(null);

  protected toggle(): void {
    this.open.update((open) => !open);
    if (this.open() && !this.defined()) void defineTimeline().then(() => this.defined.set(true));
  }

  protected onCursor(event: Event): void {
    this.cursor.set((event as VehicleSilTimelineEventMap['cursorchange']).detail.cursor);
  }

  protected onStep(event: Event): void {
    this.step.set((event as VehicleSilTimelineEventMap['stepselect']).detail.step);
  }
}
