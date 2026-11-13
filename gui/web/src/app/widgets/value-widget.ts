import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { VariableRef } from '@vehicle-sil/gui-client';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';

/** 値の表示: 最後の値と単位（列挙は項目の名前、bool はオン・オフ）。読むだけ。 */
@Component({
  selector: 'app-value-widget',
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
    </app-variable-frame>
  `,
  styles: `
    .value {
      margin: 0;
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-1);
      align-items: baseline;
    }

    .number {
      font: var(--mat-sys-headline-small);
      font-variant-numeric: tabular-nums;
      overflow-wrap: anywhere;
      min-inline-size: 0;
    }

    .unit {
      color: var(--mat-sys-on-surface-variant);
    }
  `,
})
export class ValueWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref);
  protected readonly text = computed(() => this.variable.text(this.variable.reading()?.value));
  protected readonly unit = computed(() => this.variable.info()?.unit ?? '');
}
