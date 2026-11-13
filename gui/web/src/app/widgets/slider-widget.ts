import { ChangeDetectionStrategy, Component, computed, input, linkedSignal } from '@angular/core';
import { MatSliderModule } from '@angular/material/slider';
import { VariableRef } from '@vehicle-sil/gui-client';
import { bindVariable } from './variable-binding';
import { WriteBlock } from './variable-state';

type WriteBlockKey = WriteBlock | null;
import { VariableFrame } from './variable-frame';

/**
 * スライダー: 範囲（min と max）が決まっている、書ける数。動かしている間は書かず、放したとき（キーボードでは
 * キーを押すたび）に書く。前の書き込みをサーバーが受け取る前に次を放したら、最後の値だけを送る。
 * 範囲のない変数と、値がまだない間（つまみを置く場所がない）は、操作させずに理由を出す。
 * 設定（options）: `step`（刻み。正の数）。
 */
@Component({
  selector: 'app-slider-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [MatSliderModule, VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()" [writable]="true">
      <p class="value" data-testid="widget-value">
        <span class="number">{{ text() }}</span>&ngsp;
        @if (unit()) {
          <span class="unit">{{ unit() }}</span>
        }
      </p>
      <mat-slider
        class="slider"
        [min]="min()"
        [max]="max()"
        [step]="step()"
        [disabled]="variable.block() !== null"
        [displayWith]="valueText"
      >
        <input
          #thumb
          matSliderThumb
          data-testid="widget-control"
          [value]="position()"
          [attr.aria-labelledby]="variable.id + '-name'"
          [attr.aria-describedby]="variable.id + '-why'"
          (input)="dragging.set(thumb.valueAsNumber)"
          (change)="release(thumb.valueAsNumber)"
          (blur)="dragging.set(null)"
          (pointercancel)="dragging.set(null)"
        />
      </mat-slider>
      <p class="range" aria-hidden="true">
        <span>{{ variable.text(min()) }}</span>
        <span>{{ variable.text(max()) }}</span>
      </p>
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
    }

    .unit,
    .range {
      color: var(--mat-sys-on-surface-variant);
    }

    .slider {
      display: block;
      inline-size: auto;
      min-inline-size: 0;
      margin-inline: 0;
    }

    .range {
      margin: 0;
      display: flex;
      justify-content: space-between;
      font: var(--mat-sys-body-small);
    }
  `,
})
export class SliderWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref, { needsRange: true, needsValue: true });

  protected readonly min = computed(() => this.variable.info()?.min ?? 0);
  protected readonly max = computed(() => this.variable.info()?.max ?? 100);
  /** 刻み: 設定の step、宣言の刻み、なければ整数は 1、実数は範囲の 1/100。 */
  protected readonly step = computed(() => {
    const asked = this.options()?.['step'];
    if (typeof asked === 'number' && asked > 0) return asked;
    const info = this.variable.info();
    if (info?.step && info.step > 0) return info.step;
    if (info?.type === 'integer') return 1;
    return (this.max() - this.min()) / 100;
  });
  protected readonly unit = computed(() => this.variable.info()?.unit ?? '');

  /**
   * 動かしている途中の位置（放したら null）。書けなくなったら（実行が終わった、切れた、など）捨てる: 無効にした
   * つまみには change が来ないことがあり、書いていない位置を値として出し続けないように。
   */
  protected readonly dragging = linkedSignal<WriteBlockKey, number | null>({
    source: () => this.variable.block(),
    computation: () => null,
  });
  /** 出す値: 動かしている途中の位置（書けるときだけ）、送っていて結果がまだの値、読んだ値の順。 */
  private readonly shown = computed(() => {
    const dragging = this.variable.block() === null ? this.dragging() : null;
    return dragging ?? this.variable.pending() ?? this.variable.reading()?.value;
  });
  protected readonly position = computed(() => {
    const value = this.shown();
    return typeof value === 'number' ? value : this.min();
  });
  protected readonly text = computed(() => this.variable.text(this.shown()));
  /** 読み上げる値（単位つき）。 */
  protected readonly valueText = (value: number) => {
    const unit = this.variable.info()?.unit;
    return unit ? `${this.variable.text(value)} ${unit}` : this.variable.text(value);
  };

  protected release(value: number): void {
    this.dragging.set(null);
    if (Number.isFinite(value)) this.variable.write(value);
  }
}
