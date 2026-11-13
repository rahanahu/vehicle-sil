import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { VariableRef } from '@vehicle-sil/gui-client';
import { blockText, busToggleLabel, writeText } from './labels';
import { bindVariable } from './variable-binding';
import { isWriteError } from './variable-state';

/**
 * ECU の見出しに置く「バスにつなぐ」のスイッチ（vehicle-sil がどの ECU にも付ける入力 `ecu.connected`）。
 * 外れている間（読んだ値が false）は、印と文で出す（ホストの `data-connected` が "false" になるので、まとまりの
 * 側も印を付けられる）。書き込みの扱いはほかの部品と同じ（bindVariable）。
 */
@Component({
  selector: 'app-bus-connection',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [MatSlideToggleModule],
  host: {
    '[attr.data-connected]': 'connected()',
    '[attr.data-variable]': "ref().peer + '/' + ref().name",
    'data-testid': 'bus-connection',
  },
  template: `
    <mat-slide-toggle
      data-testid="bus-toggle"
      [checked]="checked()"
      [disabled]="variable.block() !== null"
      disabledInteractive
      [aria-label]="toggleLabel()"
      [aria-describedby]="variable.id + '-state'"
      (change)="variable.write($event.checked)"
      i18n
    >
      バスにつなぐ
    </mat-slide-toggle>
    <p class="state" [id]="variable.id + '-state'" role="status" data-testid="bus-state">
      @if (connected() === 'false') {
        <span class="mark" aria-hidden="true">⚠</span>
        <ng-container i18n
          >この ECU はバスから外れています（CAN と LIN
          に送らず、受けません。ステップは動き続けます）。</ng-container
        >
      }
      @if (why(); as text) {
        {{ text }}
      }
    </p>
    <p class="state error" role="alert" data-testid="bus-error">
      @if (error(); as text) {
        <span class="mark" aria-hidden="true">⚠</span>{{ text }}
      }
    </p>
  `,
  styles: `
    :host {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-1) var(--app-space-3);
      align-items: center;
      margin-block-end: var(--app-space-2);
    }

    .state {
      margin: 0;
      font: var(--mat-sys-body-small);
      color: var(--mat-sys-on-surface-variant);
      overflow-wrap: anywhere;
    }

    .state:empty {
      display: none;
    }

    :host([data-connected='false']) .state,
    .error {
      color: var(--mat-sys-error);
    }

    .mark {
      margin-inline-end: var(--app-space-1);
    }
  `,
})
export class BusConnection {
  readonly ref = input.required<VariableRef>();
  protected readonly variable = bindVariable(this.ref);

  /** 読んだ値で: "true"、"false"、まだわからなければ null。 */
  protected readonly connected = computed(() => {
    const value = this.variable.reading()?.value;
    return typeof value === 'boolean' ? String(value) : null;
  });
  /** スイッチの位置: 送っていて結果がまだなら送った値、なければ読んだ値（わからなければ、つないでいる）。 */
  protected readonly checked = computed(() => {
    const shown = this.variable.pending() ?? this.variable.reading()?.value;
    return shown !== false;
  });
  protected readonly toggleLabel = computed(() => busToggleLabel(this.ref().peer));
  protected readonly why = computed(() => {
    const block = this.variable.block();
    return block === null || block === 'read-only' ? '' : blockText(block);
  });
  protected readonly error = computed(() => {
    const status = this.variable.writeStatus();
    return isWriteError(status) ? writeText(status).text : null;
  });
}
