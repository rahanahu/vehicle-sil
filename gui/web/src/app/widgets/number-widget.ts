import { ChangeDetectionStrategy, Component, computed, input, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { VariableRef } from '@vehicle-sil/gui-client';
import { numberProblemText } from './labels';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';
import { parseNumber } from './variable-state';

/**
 * 数値の入力: 書ける数（範囲がなくてもよい）。入れて Enter か「書く」で書く。型（整数）と範囲は送る前に確かめ、
 * 合わなければ送らずに理由を出す。いまの値は欄の上に出す（入れている途中で書き換わらない）。
 */
@Component({
  selector: 'app-number-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [MatButtonModule, MatFormFieldModule, MatInputModule, VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()" [writable]="true">
      <p class="value" data-testid="widget-value">
        <span class="number">{{ text() }}</span>&ngsp;
        @if (unit()) {
          <span class="unit">{{ unit() }}</span>
        }
      </p>
      <div class="row">
        <mat-form-field class="field" subscriptSizing="dynamic">
          <mat-label i18n>書く値</mat-label>
          <input
            #field
            matInput
            type="text"
            inputmode="decimal"
            data-testid="widget-control"
            [attr.autocomplete]="'off'"
            [value]="draft()"
            [disabled]="variable.block() !== null"
            [attr.aria-describedby]="variable.id + '-why'"
            (input)="draft.set(field.value)"
            (keydown.enter)="submit()"
          />
          @if (unit()) {
            <span matTextSuffix>{{ unit() }}</span>
          }
        </mat-form-field>
        <button
          matButton="tonal"
          type="button"
          data-testid="widget-submit"
          [disabled]="variable.block() !== null"
          [attr.aria-describedby]="variable.id + '-why'"
          (click)="submit()"
          i18n
        >
          書く
        </button>
      </div>
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

    .unit {
      color: var(--mat-sys-on-surface-variant);
    }

    .row {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-2);
      align-items: center;
    }

    .field {
      flex: 1 1 8rem;
      min-inline-size: 0;
    }
  `,
})
export class NumberWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref);
  protected readonly unit = computed(() => this.variable.info()?.unit ?? '');
  /** いまの値（送っていて結果がまだなら、送った値）。 */
  protected readonly text = computed(() =>
    this.variable.text(this.variable.pending() ?? this.variable.reading()?.value),
  );
  /** 欄に入れている文字。 */
  protected readonly draft = signal('');

  protected submit(): void {
    const info = this.variable.info();
    if (!info || this.variable.block() !== null) return;
    const parsed = parseNumber(info, this.draft());
    if ('problem' in parsed) {
      this.variable.reject(numberProblemText(parsed.problem, info));
      return;
    }
    this.variable.write(parsed.value);
  }
}
