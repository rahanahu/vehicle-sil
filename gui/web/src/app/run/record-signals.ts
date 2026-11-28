import { Component, computed, input, linkedSignal, model } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { CheckResult } from '@vehicle-sil/gui-client';

interface RecordGroup {
  participant: string;
  variables: {
    fullName: string;
    name: string;
    causality: string;
    unit: string;
    description: string;
  }[];
}

/**
 * 実行時に追加でタイムラインへ記録する信号（`<participant>.<variable>`）の選択欄。
 * 既定は折りたたまれた空状態とし、全信号を自動記録しない。
 */
@Component({
  selector: 'app-record-signals',
  imports: [MatButtonModule, MatCheckboxModule],
  template: `
    <details class="record-signals" data-testid="record-signals">
      <summary data-testid="record-signals-summary">
        <span i18n>追加で記録する信号</span>
        @if (selected().size > 0) {
          <span class="record-count" data-testid="record-signals-count" i18n
            >（{{ selected().size }} 件選択中）</span
          >
        }
      </summary>
      <div class="record-signals-body">
        <p class="hint" i18n>
          シナリオで使わない信号もタイムラインに記録します（合否判定には影響しません）。
        </p>
        @if (selectedList().length > 0) {
          <div class="record-selected" data-testid="record-signals-selected">
            @for (sig of selectedList(); track sig) {
              <span class="record-chip" data-testid="record-signal-chip">
                <code lang="en">{{ sig }}</code>
                <button
                  type="button"
                  class="record-chip-remove"
                  data-testid="record-signal-remove"
                  [attr.aria-label]="removeLabel(sig)"
                  (click)="toggle(sig, false)"
                >
                  ×
                </button>
              </span>
            }
            <button
              matButton
              type="button"
              class="record-clear"
              data-testid="record-signals-clear"
              (click)="clear()"
              i18n
            >
              すべて解除
            </button>
          </div>
        }
        @if (hasDescribed()) {
          <input
            type="search"
            class="record-filter"
            data-testid="record-signal-filter"
            [placeholder]="filterPlaceholder"
            [attr.aria-label]="filterPlaceholder"
            [value]="filter()"
            #filterInput
            (input)="filter.set(filterInput.value)"
          />
          @if (groups().length > 0) {
            <div class="record-groups" data-testid="record-signal-groups">
              @for (group of groups(); track group.participant) {
                <div class="record-group">
                  <div class="record-group-title" lang="en">{{ group.participant }}</div>
                  @for (v of group.variables; track v.fullName) {
                    <mat-checkbox
                      class="record-option"
                      [attr.data-testid]="'record-signal-option-' + v.fullName"
                      [checked]="selected().has(v.fullName)"
                      [attr.title]="v.description || null"
                      (change)="toggle(v.fullName, $event.checked)"
                    >
                      <code lang="en">{{ v.name }}</code>
                      @if (v.unit) {
                        <span class="record-meta" lang="en">({{ v.unit }})</span>
                      }
                    </mat-checkbox>
                  }
                </div>
              }
            </div>
          } @else {
            <p class="hint" data-testid="record-signals-no-match" i18n>
              条件に合う信号がありません。
            </p>
          }
        } @else {
          <p class="hint" data-testid="record-signals-empty" i18n>
            照合すると、記録できる信号の一覧が表示されます。
          </p>
        }
        <div class="record-custom-row">
          <input
            type="text"
            class="record-custom-input"
            data-testid="record-signal-custom"
            [placeholder]="customPlaceholder"
            [attr.aria-label]="customPlaceholder"
            [value]="custom()"
            #customInput
            (input)="custom.set(customInput.value)"
            (keydown.enter)="$event.preventDefault(); addCustom()"
          />
          <button
            matButton="outlined"
            type="button"
            data-testid="record-signal-add"
            [disabled]="!custom().trim()"
            (click)="addCustom()"
            i18n
          >
            追加
          </button>
        </div>
      </div>
    </details>
  `,
  styles: `
    :host {
      display: block;
      margin-block-start: var(--app-space-2);
    }

    .record-signals {
      padding: var(--app-space-2);
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-small);
    }

    .record-signals summary {
      cursor: pointer;
      font: var(--mat-sys-label-large);
      color: var(--mat-sys-on-surface);
    }

    .record-count {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-primary);
    }

    .record-signals-body {
      display: grid;
      gap: var(--app-space-2);
      margin-block-start: var(--app-space-2);
    }

    .hint {
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
      font: var(--mat-sys-body-small);
    }

    .record-selected {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--app-space-1);
    }

    .record-chip {
      display: inline-flex;
      align-items: center;
      gap: 0.25rem;
      padding: 0.125rem 0.4rem;
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-secondary-container);
      color: var(--mat-sys-on-secondary-container);
      font: var(--mat-sys-body-small);
    }

    .record-chip-remove {
      border: none;
      background: transparent;
      color: inherit;
      cursor: pointer;
      font: inherit;
      padding: 0 0.15rem;
      line-height: 1;
    }

    .record-filter,
    .record-custom-input {
      box-sizing: border-box;
      inline-size: 100%;
      min-inline-size: 0;
      padding: 0.35rem 0.5rem;
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface);
      color: var(--mat-sys-on-surface);
      font: var(--mat-sys-body-small);
    }

    .record-groups {
      display: grid;
      gap: var(--app-space-2);
      max-block-size: 14rem;
      overflow-y: auto;
      padding: var(--app-space-1);
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-small);
    }

    .record-group {
      display: grid;
      gap: 0.125rem;
    }

    .record-group-title {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
    }

    .record-option {
      display: block;
      inline-size: fit-content;
    }

    .record-meta {
      margin-inline-start: 0.25rem;
      color: var(--mat-sys-on-surface-variant);
      font: var(--mat-sys-body-small);
    }

    .record-custom-row {
      display: flex;
      align-items: center;
      gap: var(--app-space-2);
    }

    .record-custom-input {
      flex: 1 1 auto;
    }
  `,
})
export class RecordSignals {
  readonly file = input<string | null>(null);
  readonly check = input<CheckResult | null>(null);
  readonly disabled = input<ReadonlySet<string>>(new Set());
  readonly selected = model.required<ReadonlySet<string>>();

  protected readonly filter = linkedSignal({ source: this.file, computation: () => '' });
  protected readonly custom = linkedSignal({ source: this.file, computation: () => '' });
  protected readonly filterPlaceholder = $localize`信号名で絞り込み…`;
  protected readonly customPlaceholder = $localize`<participant>.<variable>`;

  protected readonly selectedList = computed(() => [...this.selected()].sort());

  protected readonly hasDescribed = computed(() => {
    const check = this.check();
    if (!check) return false;
    const disabled = this.disabled();
    return check.participants.some(
      (p) =>
        p.name !== null &&
        p.enabled !== false &&
        !disabled.has(p.name) &&
        (p.variables?.length ?? 0) > 0,
    );
  });

  protected readonly groups = computed<RecordGroup[]>(() => {
    const check = this.check();
    if (!check) return [];
    const disabled = this.disabled();
    const query = this.filter().trim().toLowerCase();
    const out: RecordGroup[] = [];
    for (const p of check.participants) {
      if (p.name === null || p.enabled === false || disabled.has(p.name) || !p.variables?.length) {
        continue;
      }
      const vars = p.variables
        .map((v) => ({
          fullName: `${p.name}.${v.name}`,
          name: v.name,
          causality: v.causality,
          unit: v.unit,
          description: v.description ?? '',
        }))
        .filter(
          (v) =>
            !query ||
            v.fullName.toLowerCase().includes(query) ||
            v.description.toLowerCase().includes(query),
        );
      if (vars.length > 0) {
        out.push({ participant: p.name, variables: vars });
      }
    }
    return out;
  });

  protected toggle(fullName: string, checked: boolean): void {
    this.selected.update((current) => {
      const next = new Set(current);
      if (checked) next.add(fullName);
      else next.delete(fullName);
      return next;
    });
  }

  protected addCustom(): void {
    const trimmed = this.custom().trim();
    if (!trimmed) return;
    this.toggle(trimmed, true);
    this.custom.set('');
  }

  protected clear(): void {
    this.selected.set(new Set());
  }

  protected removeLabel(fullName: string): string {
    return $localize`${fullName}:signal: の追加記録を外す`;
  }
}
