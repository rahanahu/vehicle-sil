import { CdkVirtualScrollViewport, ScrollingModule } from '@angular/cdk/scrolling';
import {
  Component,
  afterRenderEffect,
  computed,
  input,
  linkedSignal,
  model,
  signal,
  viewChild,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { LogLine, LogSource } from '../core/session/session-state';

function countLines(lines: readonly LogLine[]): number {
  let count = 0;
  for (const line of lines) if (!line.gap) count++;
  return count;
}

/** 1 行の高さ（px）。仮想スクロールは決まった高さで並べる（CSS の .line と同じ値）。 */
export const LOG_ROW_PX = 20;

/**
 * ログ: participant と文字で絞り、末尾に追従する（上へ戻すと追従をやめる）。行が多くても、見えている分だけを描く。
 * 追従をやめている間は、その時点の行を見せ続ける（上限で古い行が落ちても、読んでいる行が動かない）。
 * 行はランチャーが整えた英語の文をそのまま出す。
 */
@Component({
  selector: 'app-log-view',
  imports: [
    ScrollingModule,
    MatButtonModule,
    MatFormFieldModule,
    MatInputModule,
    MatSelectModule,
    MatSlideToggleModule,
  ],
  templateUrl: './log-view.html',
  styleUrl: './log-view.css',
})
export class LogView {
  /** 実行（セッション）の番号。変わったら、追従に戻る（前の実行の行を見せ続けない）。 */
  readonly run = input<number | null>(null);
  readonly lines = input.required<readonly LogLine[]>();
  /** 出どころごとの、持っている行と落とした行の数。 */
  readonly sources = input.required<Readonly<Record<string, LogSource>>>();
  /** 絞る出どころ（'' はすべて）。外から選ぶこともある（「ランチャーのログを見る」）。 */
  readonly source = model('');

  /** 絞る文字（大文字と小文字を区別しない）。 */
  protected readonly text = signal('');
  /** 末尾に追従するか。実行が変わったら、追従に戻る。 */
  protected readonly follow = linkedSignal({ source: this.run, computation: () => true });
  protected readonly rowPx = LOG_ROW_PX;
  protected readonly trackLine = (_index: number, line: LogLine) => line.key;

  private readonly viewport = viewChild.required(CdkVirtualScrollViewport);
  private lastScrollTop = 0;

  /** 出どころの一覧。行が落ちても選択肢は消えない。 */
  protected readonly sourceNames = computed(() => Object.keys(this.sources()).sort());

  /** 見せている行: 追従している間は届いた行、やめている間は、やめた時点の行（実行が変わったら、その実行の行）。 */
  protected readonly visible = linkedSignal<
    { follow: boolean; run: number | null; lines: readonly LogLine[] },
    readonly LogLine[]
  >({
    source: () => ({ follow: this.follow(), run: this.run(), lines: this.lines() }),
    computation: (source, previous) =>
      source.follow || !previous || previous.source.run !== source.run
        ? source.lines
        : previous.value,
  });

  /** 見せている行の数（落ちた行の印は数えない）。 */
  protected readonly visibleCount = computed(() => countLines(this.visible()));
  protected readonly shownCount = computed(() => countLines(this.shown()));

  /** 追従をやめた後に届いた行の数。 */
  protected readonly newer = computed(() => {
    if (this.follow()) return 0;
    const last = this.visible().at(-1)?.seq ?? -1;
    const lines = this.lines();
    let count = 0;
    for (let i = lines.length - 1; i >= 0 && lines[i].seq > last; i--) if (!lines[i].gap) count++;
    return count;
  });

  protected readonly shown = computed(() => {
    const source = this.source();
    const text = this.text().trim().toLowerCase();
    const lines = this.visible();
    if (!source && !text) return lines;
    return lines.filter(
      (line) =>
        (!source || line.source === source) && (!text || line.line.toLowerCase().includes(text)),
    );
  });

  /** 落とした行の数（絞っている出どころの分、またはすべて）。 */
  protected readonly dropped = computed(() => {
    const sources = this.sources();
    const source = this.source();
    const entries = source ? [[source, sources[source]] as const] : Object.entries(sources);
    return entries
      .filter(([, counts]) => (counts?.dropped ?? 0) > 0)
      .map(([name, counts]) => ({ name, count: counts?.dropped ?? 0 }));
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
    if (atBottom && this.newer() === 0) {
      this.follow.set(true);
    } else if (top < this.lastScrollTop - 1) {
      this.follow.set(false);
    }
    this.lastScrollTop = top;
  }

  protected setText(value: string): void {
    this.text.set(value);
  }

  protected toEnd(): void {
    this.follow.set(true);
  }
}
