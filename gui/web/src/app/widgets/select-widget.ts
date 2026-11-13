import { ChangeDetectionStrategy, Component, computed, input, viewChild } from '@angular/core';
import { MatSelect, MatSelectModule } from '@angular/material/select';
import { VariableRef } from '@vehicle-sil/gui-client';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';

/**
 * 選択肢: 列挙の変数の項目を選んで書く。一覧を開いて選んだときだけ書く（閉じたまま矢印キーや文字を押しても
 * 書かない: Tab で渡る途中やスクロールのつもりのキーで、途中の項目を ECU に書かないように）。
 */
@Component({
  selector: 'app-select-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [MatSelectModule, VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()" [writable]="true">
      <mat-select
        class="select"
        data-testid="widget-control"
        [value]="selected()"
        [placeholder]="text()"
        [disabled]="variable.block() !== null"
        [aria-labelledby]="variable.id + '-name'"
        [attr.aria-describedby]="variable.id + '-why'"
        (selectionChange)="choose($event.value)"
      >
        @for (item of items(); track item.value) {
          <mat-option [value]="item.value">{{ item.name }}</mat-option>
        }
      </mat-select>
    </app-variable-frame>
  `,
  styles: `
    .select {
      display: block;
      max-inline-size: 100%;
      padding-block: var(--app-space-2);
      border-block-end: 1px solid var(--mat-sys-outline);
    }
  `,
})
export class SelectWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref);
  private readonly select = viewChild.required(MatSelect);
  protected readonly items = computed(() => this.variable.info()?.enum ?? []);
  private readonly shown = computed(
    () => this.variable.pending() ?? this.variable.reading()?.value,
  );
  /** 選ばれている項目の値（項目にない値や、値がないときは null: placeholder に値を出す）。 */
  protected readonly selected = computed(() => {
    const value = this.shown();
    return this.items().some((item) => item.value === value) ? value : null;
  });
  protected readonly text = computed(() => this.variable.text(this.shown()));

  protected choose(value: number): void {
    // 一覧から選んだときは、閉じる前にここへ来る（MatSelect は選んでから閉じる）
    if (this.select().panelOpen) {
      this.variable.write(value);
      return;
    }
    // 閉じたままのキーで替わった: 書かずに、出している値へ戻す
    this.select().value = this.selected();
  }
}
