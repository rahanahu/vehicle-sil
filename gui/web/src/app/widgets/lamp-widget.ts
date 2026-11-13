import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { VariableRef } from '@vehicle-sil/gui-client';
import { bindVariable } from './variable-binding';
import { VariableFrame } from './variable-frame';

/** ランプ: 読むだけの真偽。点いているかを、色だけでなく形（塗りつぶし）と文字（オン・オフ）でも出す。 */
@Component({
  selector: 'app-lamp-widget',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [VariableFrame],
  template: `
    <app-variable-frame [variable]="variable" [label]="label()">
      <p class="lamp" data-testid="widget-value" [attr.data-on]="on()">
        <span class="light" [class.on]="on() === true" aria-hidden="true"></span>
        <span>{{ text() }}</span>
      </p>
    </app-variable-frame>
  `,
  styles: `
    .lamp {
      margin: 0;
      display: flex;
      gap: var(--app-space-2);
      align-items: center;
      font: var(--mat-sys-title-medium);
    }

    .light {
      inline-size: 20px;
      block-size: 20px;
      border-radius: 50%;
      border: 2px solid var(--mat-sys-outline);
      box-sizing: border-box;
      flex: none;
    }

    .light.on {
      background: var(--mat-sys-primary);
      border-color: var(--mat-sys-primary);
    }
  `,
})
export class LampWidget {
  readonly ref = input.required<VariableRef>();
  readonly label = input<string | null | undefined>(null);
  readonly options = input<Readonly<Record<string, unknown>> | null | undefined>(null);
  protected readonly variable = bindVariable(this.ref);
  /** 点いているか（値がなければ null）。 */
  protected readonly on = computed(() => {
    const value = this.variable.reading()?.value;
    return typeof value === 'boolean' ? value : null;
  });
  protected readonly text = computed(() => this.variable.text(this.variable.reading()?.value));
}
