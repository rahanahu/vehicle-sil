import { CdkVirtualScrollViewport, ScrollingModule } from '@angular/cdk/scrolling';
import { Component, afterRenderEffect, computed, input, signal, viewChild } from '@angular/core';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { LogLine } from './run-state';

/** 1 行の高さ（px）。仮想スクロールは決まった高さで並べる（CSS の .line と同じ値）。 */
export const LOG_ROW_PX = 20;

/**
 * ログ: participant と文字で絞り、末尾に追従する（上へ戻すと追従をやめる）。行が多くても、見えている分だけを描く。
 * 行はランチャーが整えた英語の文をそのまま出す。
 */
@Component({
  selector: 'app-log-view',
  imports: [
    ScrollingModule,
    MatFormFieldModule,
    MatInputModule,
    MatSelectModule,
    MatSlideToggleModule,
  ],
  templateUrl: './log-view.html',
  styleUrl: './log-view.css',
})
export class LogView {
  readonly lines = input.required<readonly LogLine[]>();
  /** 落とした古い行の数。 */
  readonly dropped = input(0);

  /** 絞る participant（'' はすべて）。 */
  protected readonly source = signal('');
  /** 絞る文字（大文字と小文字を区別しない）。 */
  protected readonly text = signal('');
  /** 末尾に追従するか。 */
  protected readonly follow = signal(true);
  protected readonly rowPx = LOG_ROW_PX;
  protected readonly trackLine = (_index: number, line: LogLine) => line.seq;

  private readonly viewport = viewChild.required(CdkVirtualScrollViewport);
  private lastScrollTop = 0;

  protected readonly sources = computed(() =>
    [...new Set(this.lines().map((line) => line.source))].sort(),
  );

  protected readonly shown = computed(() => {
    const source = this.source();
    const text = this.text().trim().toLowerCase();
    const lines = this.lines();
    if (!source && !text) return lines;
    return lines.filter(
      (line) =>
        (!source || line.source === source) && (!text || line.line.toLowerCase().includes(text)),
    );
  });

  constructor() {
    // 追従しているあいだは、描いた後に末尾へ送る（DOM の操作なので afterRenderEffect）
    afterRenderEffect(() => {
      const count = this.shown().length;
      if (this.follow() && count > 0) {
        const viewport = this.viewport();
        viewport.checkViewportSize();
        viewport.scrollToIndex(count - 1);
      }
    });
  }

  /** 利用者が上へ戻したら追従をやめ、末尾まで来たら追従に戻す。 */
  protected onScroll(): void {
    const viewport = this.viewport();
    const top = viewport.measureScrollOffset('top');
    const atBottom = viewport.measureScrollOffset('bottom') < LOG_ROW_PX;
    if (atBottom) {
      this.follow.set(true);
    } else if (top < this.lastScrollTop - 1) {
      this.follow.set(false);
    }
    this.lastScrollTop = top;
  }

  protected setText(value: string): void {
    this.text.set(value);
  }
}
