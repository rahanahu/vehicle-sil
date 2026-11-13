import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { HistoryPoint, VariableInfo, VariableRef } from '@vehicle-sil/gui-client';
import { formatMeasure } from '../core/format/measure-pipe';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';
import { hasRange } from './dashboard-spec';

/** グラフが覚える点の数。 */
export const GRAPH_POINTS = 600;

/** 描く枠の大きさ（viewBox。CSS で幅に合わせて伸ばす）。 */
const WIDTH = 300;
const HEIGHT = 100;

/** 履歴を折れ線の点にする（数は数、bool は 0 と 1。数でない値は飛ばす）。縦は範囲があれば範囲、なければ値の幅。 */
export function plot(
  points: readonly HistoryPoint[],
  info: Pick<VariableInfo, 'min' | 'max'> | undefined,
): { line: string; low: number; high: number; from: number; to: number } | null {
  const xs: number[] = [];
  const ys: number[] = [];
  for (const point of points) {
    const y = typeof point.value === 'boolean' ? Number(point.value) : point.value;
    if (y === null || !Number.isFinite(y)) continue;
    xs.push(point.sampleMs);
    ys.push(y);
  }
  if (xs.length === 0) return null;
  let low = Math.min(...ys);
  let high = Math.max(...ys);
  if (info && hasRange(info)) {
    low = Math.min(low, info.min as number);
    high = Math.max(high, info.max as number);
  }
  if (low === high) {
    low -= 1;
    high += 1;
  }
  const from = xs[0];
  const to = xs[xs.length - 1];
  const span = to - from || 1;
  const line = xs
    .map((x, i) => {
      const px = to === from ? WIDTH : ((x - from) / span) * WIDTH;
      const py = HEIGHT - ((ys[i] - low) / (high - low)) * HEIGHT;
      return `${px.toFixed(1)},${py.toFixed(1)}`;
    })
    .join(' ');
  return { line, low, high, from, to };
}

/**
 * グラフ: 数の時間の変化（横は値を読んだステップの仮想時間、縦は値）。履歴はクライアントのライブラリが持つ
 * （見えている間の、最後の GRAPH_POINTS 点）。最後の値と単位を上に出す。
 * 設定（options）: `points`（覚えて描く点の数。2 以上の整数。既定 GRAPH_POINTS）。
 */
@Component({
  selector: 'app-graph-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()">
      <p class="value" data-testid="widget-value">
        <span class="number">{{ text() }}</span>&ngsp;
        @if (unit()) {
          <span class="unit">{{ unit() }}</span>
        }
      </p>
      <figure class="graph">
        <svg
          [attr.viewBox]="'0 0 ' + width + ' ' + height"
          preserveAspectRatio="none"
          role="img"
          [attr.aria-label]="summary()"
          data-testid="widget-graph"
          [attr.data-points]="variable.history().length"
        >
          @if (plotted(); as p) {
            <polyline class="line" [attr.points]="p.line" />
          }
        </svg>
        @if (plotted(); as p) {
          <figcaption class="axes" aria-hidden="true">
            <span>{{ range() }}</span>
            <span i18n>{{ seconds(p.from) }}–{{ seconds(p.to) }} s</span>
          </figcaption>
        }
      </figure>
    </app-variable-frame>
  `,
  styles: `
    .value {
      margin: 0;
      display: flex;
      gap: var(--app-space-1);
      align-items: baseline;
    }

    .number {
      font: var(--mat-sys-title-large);
      font-variant-numeric: tabular-nums;
      overflow-wrap: anywhere;
      min-inline-size: 0;
    }

    .unit,
    .axes {
      color: var(--mat-sys-on-surface-variant);
    }

    .graph {
      margin: 0;
      display: grid;
      gap: var(--app-space-1);
    }

    svg {
      display: block;
      inline-size: 100%;
      block-size: 96px;
      border-block-end: 1px solid var(--mat-sys-outline-variant);
      border-inline-start: 1px solid var(--mat-sys-outline-variant);
    }

    .line {
      fill: none;
      stroke: var(--mat-sys-primary);
      stroke-width: 2px;
      vector-effect: non-scaling-stroke;
    }

    .axes {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      gap: var(--app-space-2);
      font: var(--mat-sys-body-small);
    }
  `,
})
export class GraphWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref, {
    history: () => {
      const asked = this.options()?.['points'];
      return typeof asked === 'number' && Number.isInteger(asked) && asked >= 2
        ? asked
        : GRAPH_POINTS;
    },
  });
  protected readonly width = WIDTH;
  protected readonly height = HEIGHT;
  protected readonly unit = computed(() => this.variable.info()?.unit ?? '');
  protected readonly text = computed(() => this.variable.text(this.variable.reading()?.value));
  protected readonly plotted = computed(() => plot(this.variable.history(), this.variable.info()));
  protected readonly range = computed(() => {
    const p = this.plotted();
    return p ? `${this.variable.text(p.low)} – ${this.variable.text(p.high)}` : '';
  });
  /** 読み上げる中身: 最後の値と単位、点の数、時間の範囲。 */
  protected readonly summary = computed(() => {
    const p = this.plotted();
    const value = this.unit() ? `${this.text()} ${this.unit()}` : this.text();
    if (!p) return $localize`グラフ: 点はまだありません`;
    const count = this.variable.history().length;
    const from = this.seconds(p.from);
    const to = this.seconds(p.to);
    return $localize`グラフ: 最後の値 ${value}:value:、${count}:count: 点、${from}:from: から ${to}:to: 秒`;
  });

  protected seconds(ms: number): string {
    return formatMeasure(ms / 1000, 1);
  }
}
