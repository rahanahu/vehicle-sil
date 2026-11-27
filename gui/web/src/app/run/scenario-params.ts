import { Component, computed, effect, input, model, output, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';
import { CheckScenario, ScenarioParamValue } from '@vehicle-sil/gui-client';
import { paramProvenanceLabel } from './labels';
import {
  ParamRow,
  buildParamRows,
  coerceSelectParamValue,
  parseParamText,
} from './scenario-param-model';

/**
 * シナリオのパラメータ確認・上書き表:
 * `check.scenario.params`・`param_meta`・`missing_params` を組み合わせて、実効値・由来バッジ・
 * 型に応じた入力欄（候補リスト / 真偽値 / 整数 / 数値 / 文字列）・個別および一括リセットを提供する。
 */
@Component({
  selector: 'app-scenario-params',
  imports: [MatButtonModule, MatFormFieldModule, MatInputModule, MatSelectModule],
  template: `
    @if (rows().length > 0) {
      <section class="params" data-testid="scenario-params-panel">
        <div class="header">
          <h3 class="heading" i18n>パラメータ</h3>
          @if (hasChanges()) {
            <button
              matButton
              type="button"
              data-testid="params-reset-all"
              [disabled]="locked()"
              (click)="resetAll()"
              i18n
            >
              すべてリセット
            </button>
          }
        </div>
        @if (missingCount() > 0) {
          <p class="missing-banner" role="status" data-testid="params-missing-banner" i18n>
            必須パラメータが {{ missingCount() }} 件未指定です。値を入力してください。
          </p>
        }
        <ul class="list" aria-label="シナリオのパラメータ" i18n-aria-label>
          @for (row of rows(); track row.name) {
            <li
              class="row"
              [class.missing]="row.missing"
              [class.overridden]="row.overridden"
              [class.invalid]="row.invalid"
              [attr.data-testid]="'param-row-' + row.name"
              [attr.data-provenance]="row.provenance"
              [attr.data-missing]="row.missing ? 'true' : null"
            >
              <div class="meta">
                <code class="name" lang="en">{{ row.name }}</code>
                <span
                  class="badge"
                  [attr.data-source]="row.provenance"
                  [attr.data-testid]="'param-badge-' + row.name"
                  >{{ provenanceText(row) }}</span
                >
                @if (row.type) {
                  <span class="type" lang="en">{{ row.type }}</span>
                }
                @if (row.overridden || hasDraft(row.name)) {
                  <button
                    matButton
                    type="button"
                    class="reset"
                    [attr.data-testid]="'param-reset-' + row.name"
                    [disabled]="locked()"
                    (click)="resetOne(row.name)"
                    i18n
                  >
                    戻す
                  </button>
                }
              </div>
              @if (row.description) {
                <p class="desc" lang="en">{{ row.description }}</p>
              }
              <div class="control">
                @switch (row.control) {
                  @case ('select') {
                    <mat-form-field class="field" subscriptSizing="dynamic">
                      <mat-select
                        [aria-label]="row.name"
                        [attr.data-testid]="'param-input-' + row.name"
                        [value]="row.effectiveValue === undefined ? null : row.displayText"
                        [disabled]="locked()"
                        (valueChange)="onSelect(row, $event)"
                      >
                        @if (row.missing) {
                          <mat-option [value]="null" disabled i18n>選択してください</mat-option>
                        }
                        @for (option of row.values ?? []; track option) {
                          <mat-option [value]="option">{{ option }}</mat-option>
                        }
                      </mat-select>
                    </mat-form-field>
                  }
                  @case ('boolean') {
                    <mat-form-field class="field" subscriptSizing="dynamic">
                      <mat-select
                        [aria-label]="row.name"
                        [attr.data-testid]="'param-input-' + row.name"
                        [value]="
                          row.effectiveValue === undefined ? null : String(row.effectiveValue)
                        "
                        [disabled]="locked()"
                        (valueChange)="onBoolean(row, $event)"
                      >
                        @if (row.missing) {
                          <mat-option [value]="null" disabled i18n>選択してください</mat-option>
                        }
                        <mat-option value="true">{{ 'true' }}</mat-option>
                        <mat-option value="false">{{ 'false' }}</mat-option>
                      </mat-select>
                    </mat-form-field>
                  }
                  @default {
                    <mat-form-field class="field" subscriptSizing="dynamic">
                      <input
                        matInput
                        type="text"
                        [attr.inputmode]="
                          row.control === 'integer'
                            ? 'numeric'
                            : row.control === 'number'
                              ? 'decimal'
                              : null
                        "
                        [attr.aria-label]="row.name"
                        [attr.aria-invalid]="row.invalid"
                        [attr.data-testid]="'param-input-' + row.name"
                        [placeholder]="row.missing ? missingPlaceholder : ''"
                        [value]="row.displayText"
                        [disabled]="locked()"
                        #inputEl
                        (input)="onTextInput(row, inputEl.value)"
                      />
                    </mat-form-field>
                  }
                }
              </div>
              @if (row.invalid) {
                <p class="error" role="status" [attr.data-testid]="'param-error-' + row.name">
                  @if (row.control === 'integer') {
                    <ng-container i18n>整数を入力してください。</ng-container>
                  } @else {
                    <ng-container i18n>数値を入力してください。</ng-container>
                  }
                </p>
              }
            </li>
          }
        </ul>
      </section>
    }
  `,
  styles: `
    :host {
      display: block;
    }
    .params {
      margin-block-start: var(--app-space-3);
      padding-block-start: var(--app-space-2);
      border-block-start: 1px solid var(--mat-sys-outline-variant);
    }
    .header {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: var(--app-space-2);
      margin-block-end: var(--app-space-2);
    }
    .heading {
      margin: 0;
      font: var(--mat-sys-title-small);
    }
    .missing-banner {
      margin: 0 0 var(--app-space-2);
      padding: var(--app-space-1) var(--app-space-2);
      border-inline-start: 3px solid var(--mat-sys-error);
      background: var(--mat-sys-error-container);
      color: var(--mat-sys-on-error-container);
      border-radius: var(--mat-sys-corner-small);
      font: var(--mat-sys-body-small);
    }
    .list {
      margin: 0;
      padding: 0;
      list-style: none;
      display: grid;
      gap: var(--app-space-2);
    }
    .row {
      display: grid;
      gap: var(--app-space-1);
      padding: var(--app-space-2);
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-lowest);
    }
    .row.missing {
      border-color: var(--mat-sys-error);
      background: var(--mat-sys-surface-container-low);
    }
    .row.overridden {
      border-inline-start: 3px solid var(--mat-sys-primary);
    }
    .meta {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--app-space-1) var(--app-space-2);
    }
    .name {
      font: var(--mat-sys-label-large);
    }
    .badge {
      padding: 1px 6px;
      border-radius: 999px;
      font: var(--mat-sys-label-small);
      background: var(--mat-sys-surface-container-high);
      color: var(--mat-sys-on-surface-variant);
    }
    .badge[data-source='override'] {
      background: var(--mat-sys-primary-container);
      color: var(--mat-sys-on-primary-container);
    }
    .badge[data-source='missing'] {
      background: var(--mat-sys-error-container);
      color: var(--mat-sys-on-error-container);
    }
    .badge[data-source='case'],
    .badge[data-source='variant'] {
      background: var(--mat-sys-secondary-container);
      color: var(--mat-sys-on-secondary-container);
    }
    .type {
      font: var(--mat-sys-label-small);
      color: var(--mat-sys-on-surface-variant);
    }
    .reset {
      margin-inline-start: auto;
    }
    .desc {
      margin: 0;
      font: var(--mat-sys-body-small);
      color: var(--mat-sys-on-surface-variant);
    }
    .field {
      inline-size: 100%;
    }
    .error {
      margin: 0;
      font: var(--mat-sys-body-small);
      color: var(--mat-sys-error);
    }
  `,
})
export class ScenarioParamsPanel {
  readonly scenario = input<CheckScenario | null>(null);
  /** ユーザーが上書きしたパラメータ（正しい JSON 型）。 */
  readonly overrides = model<Readonly<Record<string, ScenarioParamValue>>>({});
  /** 実行中などで編集できないとき。 */
  readonly locked = input(false);
  /** テキスト入力中の不正な値があるか。 */
  readonly invalidChange = output<boolean>();

  private readonly drafts = signal<Readonly<Record<string, string>>>({});
  private lastScenarioKey: string | null = null;
  private lastOwnOverrides: Readonly<Record<string, ScenarioParamValue>> | null = null;

  protected readonly missingPlaceholder = $localize`必須`;
  protected readonly String = String;

  protected readonly rows = computed<ParamRow[]>(() =>
    buildParamRows(this.scenario(), this.overrides(), this.drafts()),
  );
  protected readonly missingCount = computed(() => this.rows().filter((row) => row.missing).length);
  protected readonly hasInvalid = computed(() => this.rows().some((row) => row.invalid));
  protected readonly hasChanges = computed(
    () => Object.keys(this.overrides()).length > 0 || Object.keys(this.drafts()).length > 0,
  );

  constructor() {
    effect(() => {
      const sc = this.scenario();
      const key = sc ? `${sc.file}\0${sc.case ?? ''}` : null;
      if (key !== this.lastScenarioKey) {
        this.lastScenarioKey = key;
        this.drafts.set({});
      }
    });
    effect(() => {
      const current = this.overrides();
      if (current === this.lastOwnOverrides) return;
      this.lastOwnOverrides = current;
      if (Object.keys(this.drafts()).length > 0) {
        this.drafts.set({});
      }
    });
    effect(() => {
      this.invalidChange.emit(this.hasInvalid());
    });
  }

  protected provenanceText(row: ParamRow): string {
    return paramProvenanceLabel(row.provenance, row.rawFrom);
  }

  protected hasDraft(name: string): boolean {
    return Object.hasOwn(this.drafts(), name);
  }

  protected onSelect(row: ParamRow, value: string | null): void {
    if (value === null) return;
    this.clearDraft(row.name);
    const typed = coerceSelectParamValue(value, row.type, row.resolvedValue);
    this.setOverride(row.name, typed);
  }

  protected onBoolean(row: ParamRow, value: string | null): void {
    if (value !== 'true' && value !== 'false') return;
    this.clearDraft(row.name);
    this.setOverride(row.name, value === 'true');
  }

  protected onTextInput(row: ParamRow, raw: string): void {
    this.drafts.update((current) => ({ ...current, [row.name]: raw }));
    const parsed = parseParamText(raw, row.control, row.type, row.resolvedValue);
    if (parsed.ok) {
      this.setOverride(row.name, parsed.value);
    }
  }

  protected resetOne(name: string): void {
    this.clearDraft(name);
    const current = this.overrides();
    if (!Object.hasOwn(current, name)) return;
    const next = { ...current };
    delete next[name];
    this.lastOwnOverrides = next;
    this.overrides.set(next);
  }

  protected resetAll(): void {
    this.drafts.set({});
    if (Object.keys(this.overrides()).length > 0) {
      const next: Readonly<Record<string, ScenarioParamValue>> = {};
      this.lastOwnOverrides = next;
      this.overrides.set(next);
    }
  }

  private clearDraft(name: string): void {
    this.drafts.update((current) => {
      if (!Object.hasOwn(current, name)) return current;
      const next = { ...current };
      delete next[name];
      return next;
    });
  }

  private setOverride(name: string, value: ScenarioParamValue): void {
    const next = { ...this.overrides(), [name]: value };
    this.lastOwnOverrides = next;
    this.overrides.set(next);
  }
}
