import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { VariableRef } from '@vehicle-sil/gui-client';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';

/**
 * スイッチ: 書ける真偽。押すとすぐに書く。結果を待つ間は送った値を出し、反映されなければ読んだ値に戻る。
 * 状態は色だけでなく文字（オン・オフ）でも出す。
 */
@Component({
  selector: 'app-switch-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [MatSlideToggleModule, VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()" [writable]="true">
      <mat-slide-toggle
        data-testid="widget-control"
        [checked]="checked()"
        [disabled]="variable.block() !== null"
        disabledInteractive
        [aria-labelledby]="variable.id + '-name'"
        [aria-describedby]="variable.id + '-why'"
        (change)="write($event.checked)"
      >
        <span data-testid="widget-value">{{ text() }}</span>
      </mat-slide-toggle>
    </app-variable-frame>
  `,
})
export class SwitchWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref);

  /** 出す値: 送っていて結果がまだなら送った値、なければ読んだ値。 */
  private readonly shown = computed(
    () => this.variable.pending() ?? this.variable.reading()?.value,
  );
  protected readonly checked = computed(() => this.shown() === true);
  protected readonly text = computed(() => this.variable.text(this.shown()));

  protected write(checked: boolean): void {
    this.variable.write(checked);
  }
}
