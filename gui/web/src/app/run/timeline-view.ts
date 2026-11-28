import {
  CUSTOM_ELEMENTS_SCHEMA,
  Component,
  ElementRef,
  computed,
  effect,
  inject,
  input,
  signal,
  viewChild,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import type {
  VehicleSilTimelineElement,
  VehicleSilTimelineEventMap,
  VehicleSilTimelineLoadDetail,
  VehicleSilTimelineLoadFailure,
  VehicleSilTimelineLoadVariable,
} from '@vehicle-sil/gui-client/timeline';
import { AuthToken } from '../core/auth/auth-token';
import { LanguageChoice } from '../core/i18n/language-choice';

/** 要素 <vehicle-sil-timeline> を定義するスクリプトを、初めて表示するときに 1 度だけ読む（初めの読み込みを重くしない）。 */
let defining: Promise<unknown> | null = null;
const defineTimeline = (): Promise<unknown> =>
  (defining ??= import('@vehicle-sil/gui-client/timeline'));

export type SignalPreset = 'all' | 'related' | 'changed' | 'inputs' | 'outputs';

/**
 * 実行の時間の軸を、この画面の中に要素 <vehicle-sil-timeline> で出す（開閉できる）。
 * 失敗要約ストリップ、信号検索・プリセット・participant 絞り込み、非表示信号のインライン復元チップを備え、
 * 信号の表示切り替えでズーム範囲・カーソル位置・選択イベントをリセットしない。
 */
@Component({
  selector: 'app-timeline-view',
  imports: [MatButtonModule],
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  host: {
    '(keydown)': 'onHostKeydown($event)',
  },
  template: `
    <div class="toggle-row">
      <button
        mat-stroked-button
        type="button"
        data-testid="timeline-toggle"
        [attr.aria-expanded]="open()"
        (click)="toggle()"
      >
        @if (open()) {
          <ng-container i18n>時間の軸を閉じる</ng-container>
        } @else {
          <ng-container i18n>時間の軸をここに表示</ng-container>
        }
      </button>
    </div>

    @if (mounted() && defined()) {
      <div class="analyzer-body" [hidden]="!open()">
        @if (failure(); as f) {
          <div class="failure-strip" role="status" data-testid="timeline-failure-strip">
            <div class="failure-info">
              <span class="failure-badge" aria-hidden="true">✕</span>
              <strong class="failure-head">{{ failureHeading(f) }}</strong>
              @if (failureSummary(f); as detail) {
                <span class="failure-detail" lang="en" [title]="detail">{{ detail }}</span>
              }
            </div>
            <div class="failure-actions">
              <button
                mat-flat-button
                type="button"
                class="jump-failure-btn"
                data-testid="timeline-jump-failure"
                (click)="jumpToFailure()"
                i18n
              >
                失敗時刻へ移動
              </button>
              @if (f.at !== null) {
                <div class="window-group" role="group" [attr.aria-label]="preFailureLabel">
                  <span class="group-label" i18n>失敗前:</span>
                  <button
                    mat-stroked-button
                    type="button"
                    class="compact-btn"
                    data-testid="timeline-window-150"
                    (click)="showPreFailureWindow(150)"
                  >
                    {{ '-150 ms' }}
                  </button>
                  <button
                    mat-stroked-button
                    type="button"
                    class="compact-btn"
                    data-testid="timeline-window-300"
                    (click)="showPreFailureWindow(300)"
                  >
                    {{ '-300 ms' }}
                  </button>
                  <button
                    mat-stroked-button
                    type="button"
                    class="compact-btn"
                    data-testid="timeline-window-all"
                    (click)="showWholeRun()"
                    i18n
                  >
                    全体
                  </button>
                </div>
              }
            </div>
          </div>
        }

        @if (allVariables().length > 0) {
          <div class="filter-bar" data-testid="timeline-filter-bar">
            <div class="filter-group search-group">
              <input
                #searchInput
                type="text"
                class="search-input"
                data-testid="timeline-signal-search"
                [attr.aria-label]="searchLabel"
                [placeholder]="searchPlaceholder"
                [value]="searchQuery()"
                (input)="onSearchInput($event)"
              />
              @if (searchQuery().length > 0) {
                <button
                  type="button"
                  class="search-clear"
                  data-testid="timeline-signal-search-clear"
                  [attr.aria-label]="clearSearchLabel"
                  (click)="clearSearch()"
                >
                  ×
                </button>
              }
            </div>

            <span class="divider" aria-hidden="true"></span>

            <div class="filter-group preset-group" role="group" [attr.aria-label]="presetLabel">
              <button
                type="button"
                class="chip-btn"
                data-testid="timeline-preset-all"
                [attr.aria-pressed]="preset() === 'all'"
                (click)="setPreset('all')"
                i18n
              >
                すべて
              </button>
              @if (failure()) {
                <button
                  type="button"
                  class="chip-btn"
                  data-testid="timeline-preset-related"
                  [attr.aria-pressed]="preset() === 'related'"
                  (click)="setPreset('related')"
                  i18n
                >
                  失敗に関係
                </button>
                <button
                  type="button"
                  class="chip-btn"
                  data-testid="timeline-preset-changed"
                  [attr.aria-pressed]="preset() === 'changed'"
                  (click)="setPreset('changed')"
                  i18n
                >
                  失敗直前に変化
                </button>
              }
              <button
                type="button"
                class="chip-btn"
                data-testid="timeline-preset-inputs"
                [attr.aria-pressed]="preset() === 'inputs'"
                (click)="setPreset('inputs')"
                i18n
              >
                入力
              </button>
              <button
                type="button"
                class="chip-btn"
                data-testid="timeline-preset-outputs"
                [attr.aria-pressed]="preset() === 'outputs'"
                (click)="setPreset('outputs')"
                i18n
              >
                出力
              </button>
            </div>

            @if (participants().length >= 2) {
              <span class="divider" aria-hidden="true"></span>
              <div
                class="filter-group participant-group"
                role="group"
                [attr.aria-label]="participantLabel"
              >
                @for (p of participants(); track p) {
                  <button
                    type="button"
                    class="chip-btn"
                    [attr.data-testid]="'timeline-participant-' + p"
                    [attr.aria-pressed]="!hiddenParticipants().has(p)"
                    (click)="toggleParticipant(p)"
                  >
                    {{ p }}
                  </button>
                }
              </div>
            }

            <span class="visible-count" data-testid="timeline-visible-count">
              {{ visibleVariables().length }} / {{ allVariables().length }}
            </span>
          </div>

          @if (hasHiddenVariables()) {
            <div class="hidden-strip" data-testid="timeline-hidden-strip">
              <span class="group-label" i18n>非表示（{{ hiddenVariablesList().length }}）:</span>
              @for (name of hiddenVariablesList(); track name) {
                <button
                  type="button"
                  class="restore-chip"
                  data-testid="timeline-restore-var"
                  [attr.data-variable]="name"
                  (click)="restoreVariable(name)"
                >
                  {{ name }} ↺
                </button>
              }
              <button
                type="button"
                class="restore-all-btn"
                data-testid="timeline-restore-all"
                (click)="restoreAllVariables()"
                i18n
              >
                すべて戻す
              </button>
            </div>
          }

          @if (noMatchingVariables()) {
            <div class="empty-filter" role="status" data-testid="timeline-filter-empty">
              <span i18n>条件に合う信号がありません。</span>
              <button
                mat-stroked-button
                type="button"
                class="compact-btn"
                data-testid="timeline-filter-reset"
                (click)="resetFilters()"
                i18n
              >
                絞り込みを解除
              </button>
            </div>
          }
        }

        <vehicle-sil-timeline
          #timelineEl
          data-testid="timeline-element"
          [src]="src()"
          [lang]="language"
          [token]="token()"
          [compact]="true"
          [hide]="hidden"
          [variables]="boundVariables()"
          (load)="onLoad($event)"
          (cursorchange)="onCursor($event)"
          (stepselect)="onStep($event)"
          (variablehide)="onVariableHide($event)"
        />

        <p class="readout" data-testid="timeline-readout">
          @if (cursor() !== null) {
            <span i18n>カーソル: {{ cursor()?.toFixed(1) }} ms</span>
          }
          @if (step() !== null) {
            <span i18n>選んだ手順: {{ step() }}</span>
          }
        </p>
      </div>
    }
  `,
  styles: `
    :host {
      display: block;
      margin: 0 0 var(--app-space-2);
    }
    .toggle-row,
    .failure-actions,
    .window-group,
    .filter-group,
    .readout {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 4px var(--app-space-2);
    }
    .window-group,
    .filter-group {
      gap: 4px;
      min-inline-size: 0;
    }
    .analyzer-body {
      margin-block-start: var(--app-space-2);
      display: grid;
      gap: var(--app-space-2);
      min-inline-size: 0;
    }
    .analyzer-body[hidden] {
      display: none;
    }
    .failure-strip,
    .filter-bar,
    .hidden-strip,
    .empty-filter {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 6px 8px;
      padding: 6px 10px;
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-low);
      min-inline-size: 0;
    }
    .failure-strip {
      justify-content: space-between;
      padding: var(--app-space-2) var(--app-space-3);
      border-color: var(--mat-sys-error);
      border-inline-start-width: 4px;
      background: var(--mat-sys-error-container);
      color: var(--mat-sys-on-error-container);
    }
    .failure-info {
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: 4px var(--app-space-2);
      min-inline-size: 0;
      flex: 1 1 260px;
    }
    .failure-badge {
      font-weight: 700;
      color: var(--mat-sys-error);
    }
    .failure-head {
      white-space: nowrap;
      font: var(--mat-sys-title-small);
    }
    .failure-detail {
      font:
        12px/1.4 ui-monospace,
        'Cascadia Mono',
        Menlo,
        Consolas,
        monospace;
      overflow-wrap: anywhere;
    }
    .group-label {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
      white-space: nowrap;
    }
    .failure-strip .group-label {
      color: inherit;
    }
    .compact-btn {
      min-inline-size: 0;
      padding-inline: 10px;
    }
    .search-group {
      position: relative;
      flex: 1 1 180px;
      max-inline-size: 280px;
    }
    .search-input {
      box-sizing: border-box;
      inline-size: 100%;
      padding: 4px 26px 4px 8px;
      border: 1px solid var(--mat-sys-outline);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface);
      color: var(--mat-sys-on-surface);
      font: var(--mat-sys-body-medium);
    }
    .search-clear {
      position: absolute;
      inset-inline-end: 4px;
      inset-block-start: 50%;
      transform: translateY(-50%);
      border: none;
      background: transparent;
      color: var(--mat-sys-on-surface-variant);
      cursor: pointer;
      padding: 0 4px;
      font-size: 16px;
      line-height: 1;
    }
    .divider {
      inline-size: 1px;
      block-size: 20px;
      background: var(--mat-sys-outline-variant);
    }
    .chip-btn,
    .restore-chip {
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: 14px;
      padding: 3px 10px;
      background: var(--mat-sys-surface);
      color: var(--mat-sys-on-surface);
      font: var(--mat-sys-label-medium);
      cursor: pointer;
    }
    .chip-btn {
      white-space: nowrap;
    }
    .chip-btn[aria-pressed='true'] {
      background: var(--mat-sys-secondary-container);
      color: var(--mat-sys-on-secondary-container);
      border-color: var(--mat-sys-primary);
      font-weight: 600;
    }
    .search-input:focus-visible,
    .chip-btn:focus-visible,
    .restore-chip:focus-visible,
    .restore-all-btn:focus-visible {
      outline: 2px solid var(--mat-sys-primary);
      outline-offset: 2px;
    }
    .visible-count {
      margin-inline-start: auto;
      font: var(--mat-sys-label-medium);
      font-variant-numeric: tabular-nums;
      color: var(--mat-sys-on-surface-variant);
      white-space: nowrap;
    }
    .hidden-strip {
      border-style: dashed;
      background: var(--mat-sys-surface-container-lowest);
    }
    .restore-chip {
      padding: 2px 8px;
      font-family: ui-monospace, 'Cascadia Mono', Menlo, Consolas, monospace;
      overflow-wrap: anywhere;
    }
    .restore-all-btn {
      border: none;
      background: none;
      padding: 2px 4px;
      color: var(--mat-sys-primary);
      font: var(--mat-sys-label-medium);
      text-decoration: underline;
      cursor: pointer;
    }
    .empty-filter,
    .readout {
      color: var(--mat-sys-on-surface-variant);
    }
    .readout {
      margin: 0;
    }
    vehicle-sil-timeline {
      inline-size: 100%;
      max-block-size: 78vh;
      border: 1px solid var(--mat-sys-outline-variant);
      border-radius: 6px;
    }
  `,
})
export class TimelineView {
  protected readonly language = inject(LanguageChoice).current;
  protected readonly token = inject(AuthToken).token;

  /** サーバーの、実行の時間の軸のファイル（`api/timeline/<id>`。サーバーの根からの相対）。 */
  readonly timelineUrl = input.required<string>();
  /** 失敗したシナリオなど、初めてこの timelineUrl を開くときに自動展開するか。 */
  readonly autoExpand = input(false);
  /** 自動展開時にフォーカスする 1 始まりの手順番号（なければ null）。 */
  readonly initialStep = input<number | null>(null);

  private readonly timelineEl = viewChild<ElementRef<VehicleSilTimelineElement>>('timelineEl');
  private readonly searchInput = viewChild<ElementRef<HTMLInputElement>>('searchInput');
  private lastUrl: string | null = null;
  private autoExpandedUrl: string | null = null;
  private pendingStep: number | null = null;
  private pendingAutoFailure = false;

  /** 標準 GUI では重複する見出し・要約・操作説明・表を要素側で隠す。 */
  protected readonly hidden = 'header summary help tables';
  protected readonly src = computed(() => `/${this.timelineUrl()}`);
  protected readonly open = signal(false);
  protected readonly mounted = signal(false);
  protected readonly defined = signal(false);

  /** 表示用（要素の事象で受けるだけ。プロパティに書き戻してループさせない）。 */
  protected readonly cursor = signal<number | null>(null);
  protected readonly step = signal<number | null>(null);
  protected readonly meta = signal<VehicleSilTimelineLoadDetail | null>(null);

  /** 信号フィルタ状態（表示専用。ズーム・カーソル・選択イベントを変えない）。 */
  protected readonly searchQuery = signal('');
  protected readonly preset = signal<SignalPreset>('all');
  protected readonly hiddenVariables = signal<ReadonlySet<string>>(new Set());
  protected readonly hiddenParticipants = signal<ReadonlySet<string>>(new Set());

  protected readonly searchLabel = $localize`信号を検索`;
  protected readonly searchPlaceholder = $localize`信号名で絞り込み… (/)`;
  protected readonly clearSearchLabel = $localize`検索を消す`;
  protected readonly presetLabel = $localize`信号の絞り込み`;
  protected readonly participantLabel = $localize`participant で絞り込み`;
  protected readonly preFailureLabel = $localize`失敗前の表示範囲`;

  protected readonly allVariables = computed<VehicleSilTimelineLoadVariable[]>(
    () => this.meta()?.variables ?? [],
  );
  protected readonly failure = computed<VehicleSilTimelineLoadFailure | null>(
    () => this.meta()?.failure ?? null,
  );
  protected readonly participants = computed<string[]>(() => [
    ...new Set(this.allVariables().map((v) => v.participant)),
  ]);
  protected readonly hiddenVariablesList = computed<string[]>(() => [...this.hiddenVariables()]);
  protected readonly hasHiddenVariables = computed(() => this.hiddenVariables().size > 0);
  protected readonly noMatchingVariables = computed(
    () => this.allVariables().length > 0 && this.visibleVariables().length === 0,
  );

  protected readonly visibleVariables = computed<VehicleSilTimelineLoadVariable[]>(() => {
    const vars = this.allVariables();
    const preset = this.preset();
    const query = this.searchQuery().trim().toLowerCase();
    const hiddenVars = this.hiddenVariables();
    const hiddenParts = this.hiddenParticipants();
    return vars.filter((v) => {
      if (hiddenVars.has(v.name)) return false;
      if (hiddenParts.has(v.participant)) return false;
      if (preset === 'related' && !v.relatedToFailure) return false;
      if (preset === 'changed' && !v.changedNearFailure && !v.failing) return false;
      if (preset === 'inputs' && !v.written && v.causality !== 'input') return false;
      if (
        preset === 'outputs' &&
        (v.causality === 'input' || (v.written && v.causality !== 'output'))
      )
        return false;
      if (
        query &&
        !v.name.toLowerCase().includes(query) &&
        !v.participant.toLowerCase().includes(query) &&
        !v.unit.toLowerCase().includes(query)
      ) {
        return false;
      }
      return true;
    });
  });

  protected readonly boundVariables = computed<string[] | null>(() => {
    if (!this.meta()) return null;
    if (
      this.preset() === 'all' &&
      this.searchQuery().trim() === '' &&
      this.hiddenVariables().size === 0 &&
      this.hiddenParticipants().size === 0
    ) {
      return null;
    }
    return this.visibleVariables().map((v) => v.name);
  });

  constructor() {
    effect(() => {
      const url = this.timelineUrl();
      const shouldExpand = this.autoExpand();
      const targetStep = this.initialStep();
      if (url !== this.lastUrl) {
        this.lastUrl = url;
        this.autoExpandedUrl = null;
        this.pendingStep = null;
        this.pendingAutoFailure = shouldExpand;
        this.cursor.set(null);
        this.step.set(shouldExpand ? targetStep : null);
        this.meta.set(null);
        this.searchQuery.set('');
        this.preset.set('all');
        this.hiddenVariables.set(new Set());
        this.hiddenParticipants.set(new Set());
        this.open.set(shouldExpand);
        this.mounted.set(shouldExpand);
        if (shouldExpand) {
          this.autoExpandedUrl = url;
          this.ensureDefined();
        }
        return;
      }
      if (shouldExpand && this.autoExpandedUrl !== url) {
        this.autoExpandedUrl = url;
        this.pendingAutoFailure = true;
        this.step.set(targetStep);
        this.open.set(true);
        this.mounted.set(true);
        this.ensureDefined();
      }
    });
  }

  /** タイムラインを展開し、指定した 1 始まりの手順へフォーカスする（拡大中はズーム幅を維持）。 */
  showStep(index: number): void {
    this.open.set(true);
    this.mounted.set(true);
    this.ensureDefined();
    this.step.set(index);
    const el = this.timelineEl()?.nativeElement;
    if (el && this.meta()) {
      el.focusStep(index, { preserveZoom: true });
      el.scrollIntoView({ block: 'nearest' });
    } else {
      this.pendingStep = index;
    }
  }

  /** 失敗時刻へ移動する（拡大中はズーム幅を維持して失敗時刻を右寄りに配置）。 */
  protected jumpToFailure(): void {
    const el = this.timelineEl()?.nativeElement;
    if (!el) return;
    el.jumpToFailure({ preserveZoom: true });
    el.scrollIntoView({ block: 'nearest' });
  }

  /** 失敗前の明示的な時間窓（-150 ms / -300 ms など）に切り替え、カーソルを失敗時刻に固定する。 */
  protected showPreFailureWindow(leadMs: number): void {
    const el = this.timelineEl()?.nativeElement;
    const m = this.meta();
    const f = m?.failure;
    if (!el || !m || !f || f.at === null) return;
    const tail = leadMs * 0.25;
    const from = Math.max(m.start, f.at - leadMs);
    const to = Math.min(m.end, f.at + tail);
    el.from = from;
    el.to = Math.max(to, from + 1);
    el.jumpToFailure({ preserveZoom: true });
  }

  protected showWholeRun(): void {
    const el = this.timelineEl()?.nativeElement;
    const m = this.meta();
    if (!el || !m) return;
    el.from = m.start;
    el.to = m.end;
  }

  protected failureHeading(f: VehicleSilTimelineLoadFailure): string {
    const at = f.at !== null ? `${f.at} ms` : '—';
    return f.name ? `${at} (#${f.step} ${f.name})` : `${at} (#${f.step})`;
  }

  protected failureSummary(f: VehicleSilTimelineLoadFailure): string {
    if (f.conditions.length > 0) {
      return f.conditions
        .map((c) => {
          const left = [c.variable, c.compare].filter(Boolean).join(' ');
          const got = c.valueText !== null ? ` = ${c.valueText}` : '';
          const rhs = c.compared
            .map((r) => `${r.variable ?? '?'}=${r.valueText ?? '?'}`)
            .join(', ');
          return rhs ? `${left}${got} (${rhs})` : `${left}${got}`;
        })
        .join('; ');
    }
    return f.message ?? f.text;
  }

  protected toggle(): void {
    const next = !this.open();
    this.open.set(next);
    if (next) {
      this.mounted.set(true);
      this.ensureDefined();
    }
  }

  protected onSearchInput(event: Event): void {
    this.searchQuery.set((event.target as HTMLInputElement).value);
  }

  protected clearSearch(): void {
    this.searchQuery.set('');
  }

  protected setPreset(preset: SignalPreset): void {
    this.preset.set(preset);
  }

  protected toggleParticipant(participant: string): void {
    this.hiddenParticipants.update((prev) => {
      const next = new Set(prev);
      if (next.has(participant)) next.delete(participant);
      else next.add(participant);
      return next;
    });
  }

  protected restoreVariable(name: string): void {
    this.hiddenVariables.update((prev) => {
      const next = new Set(prev);
      next.delete(name);
      return next;
    });
  }

  protected restoreAllVariables(): void {
    this.hiddenVariables.set(new Set());
  }

  protected resetFilters(): void {
    this.searchQuery.set('');
    this.preset.set('all');
    this.hiddenVariables.set(new Set());
    this.hiddenParticipants.set(new Set());
  }

  protected onLoad(event: Event): void {
    const detail = (event as VehicleSilTimelineEventMap['load']).detail;
    this.meta.set(detail);
    const el = this.timelineEl()?.nativeElement;
    if (!el) return;
    if (this.pendingStep !== null) {
      const idx = this.pendingStep;
      this.pendingStep = null;
      this.pendingAutoFailure = false;
      el.focusStep(idx, { preserveZoom: false });
      return;
    }
    if (this.pendingAutoFailure) {
      this.pendingAutoFailure = false;
      if (detail.failure) {
        el.jumpToFailure({ preserveZoom: false });
      } else {
        const target = this.initialStep();
        if (target !== null) el.focusStep(target, { preserveZoom: false });
      }
    }
  }

  protected onCursor(event: Event): void {
    this.cursor.set((event as VehicleSilTimelineEventMap['cursorchange']).detail.cursor);
  }

  protected onStep(event: Event): void {
    this.step.set((event as VehicleSilTimelineEventMap['stepselect']).detail.step);
  }

  protected onVariableHide(event: Event): void {
    event.preventDefault();
    const name = (event as VehicleSilTimelineEventMap['variablehide']).detail.variable;
    this.hiddenVariables.update((prev) => {
      const next = new Set(prev);
      next.add(name);
      return next;
    });
  }

  protected onHostKeydown(event: KeyboardEvent): void {
    if (!this.open() || event.ctrlKey || event.metaKey || event.altKey) return;
    const target = event.target as HTMLElement | null;
    const inField =
      target !== null &&
      (target.tagName === 'INPUT' ||
        target.tagName === 'TEXTAREA' ||
        target.tagName === 'SELECT' ||
        target.isContentEditable);
    if (inField) {
      if (event.key === 'Escape' && this.searchQuery().length > 0) {
        event.preventDefault();
        this.clearSearch();
      }
      return;
    }
    if (event.key === '/' && this.allVariables().length > 0) {
      event.preventDefault();
      this.searchInput()?.nativeElement.focus();
    } else if ((event.key === 'f' || event.key === 'F') && this.failure()) {
      event.preventDefault();
      this.jumpToFailure();
    }
  }

  private ensureDefined(): void {
    if (!this.defined()) {
      void defineTimeline().then(() => this.defined.set(true));
    }
  }
}
