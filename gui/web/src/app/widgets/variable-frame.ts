import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { isWriteError } from '@vehicle-sil/gui-client';
import { blockText, conditionText, roleLabel, writeText } from './labels';
import { VariableBinding } from './variable-binding';

/**
 * 部品の枠: 変数の名前、役割、単位、中身（部品が差し込む）、値の状態、書き込みの様子、書けない理由。どの部品も
 * この枠に入れる（自分で作る部品も）。読み上げでは、枠が 1 つのまとまり（名前つき）になる。
 *
 * 中身の操作には `[attr.aria-labelledby]="variable.id + '-name'"`（名前）と
 * `[attr.aria-describedby]="variable.id + '-why'"`（書けない理由）を付ける。
 */
@Component({
  selector: 'app-variable-frame',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section
      class="frame"
      role="group"
      [attr.aria-labelledby]="variable().id + '-name'"
      [attr.data-condition]="variable().condition().kind"
      [attr.data-write]="variable().writeStatus().kind"
      [attr.data-variable]="variable().ref().peer + '/' + variable().ref().name"
    >
      <header class="head">
        <span class="name" [id]="variable().id + '-name'" [attr.title]="description()">
          {{ label() ?? variable().ref().name }}
        </span>
        @if (role()) {
          <span class="role">{{ role() }}</span>
        }
      </header>
      <div class="body">
        <ng-content />
      </div>
      <p class="condition" data-testid="widget-condition">
        @if (condition().mark) {
          <span class="mark" aria-hidden="true">{{ condition().mark }}</span>
        }
        {{ condition().text }}
      </p>
      @if (writable()) {
        <!-- 書き込みの誤りは alert、それ以外の様子は status。領域は中身が変わる前から置いておく -->
        <p class="write" role="status" data-testid="widget-write">
          @if (!failed()) {
            @if (write().mark) {
              <span class="mark" aria-hidden="true">{{ write().mark }}</span>
            }
            {{ write().text }}
          }
        </p>
        <p class="write error" role="alert" data-testid="widget-error">
          @if (failed()) {
            <span class="mark" aria-hidden="true">{{ write().mark }}</span>
            {{ write().text }}
            @if (write().english; as english) {
              <span lang="en">{{ english }}</span>
            }
          }
        </p>
        <p class="why" [id]="variable().id + '-why'" data-testid="widget-why">{{ why() }}</p>
      }
    </section>
  `,
  styles: `
    :host {
      display: block;
      min-inline-size: 0;
    }

    .frame {
      display: grid;
      gap: var(--app-space-1);
      padding: var(--app-space-3);
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-medium);
      background: var(--mat-sys-surface);
      min-inline-size: 0;
    }

    .head {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-2);
      align-items: baseline;
      min-inline-size: 0;
    }

    .name {
      font: var(--mat-sys-title-small);
      overflow-wrap: anywhere;
      min-inline-size: 0;
    }

    .role {
      font: var(--mat-sys-label-small);
      color: var(--mat-sys-on-surface-variant);
    }

    .body {
      min-inline-size: 0;
    }

    p {
      margin: 0;
      font: var(--mat-sys-body-small);
      color: var(--mat-sys-on-surface-variant);
      overflow-wrap: anywhere;
    }

    p:empty {
      display: none;
    }

    .error {
      color: var(--mat-sys-error);
    }

    [data-condition='stale'] .condition,
    [data-condition='missing'] .condition {
      color: var(--mat-sys-error);
    }

    .mark {
      margin-inline-end: var(--app-space-1);
    }
  `,
})
export class VariableFrame {
  /** 部品のつながり（bindVariable の戻り値）。 */
  readonly variable = input.required<VariableBinding>();
  /** 見出しに出す名前（省くと変数の名前）。 */
  readonly label = input<string | null | undefined>(null);
  /** 書く部品か（書き込みの様子と、書けない理由の欄を出す）。 */
  readonly writable = input(false);

  protected readonly role = computed(() => roleLabel(this.variable().info()));
  protected readonly description = computed(() => this.variable().info()?.description || null);
  protected readonly condition = computed(() => conditionText(this.variable().condition()));
  protected readonly write = computed(() => writeText(this.variable().writeStatus()));
  protected readonly failed = computed(() => isWriteError(this.variable().writeStatus()));
  protected readonly why = computed(() => {
    const block = this.variable().block();
    return block === null || block === 'read-only' ? '' : blockText(block);
  });
}
