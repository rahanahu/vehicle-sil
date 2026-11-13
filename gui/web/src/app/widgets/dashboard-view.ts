import { NgComponentOutlet } from '@angular/common';
import {
  ChangeDetectionStrategy,
  Component,
  Type,
  computed,
  inject,
  input,
  signal,
} from '@angular/core';
import { EcuVariables } from '../core/session/ecu-variables';
import { DashboardSpec, ItemProblem, WidgetKind, checkItem } from './dashboard-spec';
import { itemProblemText } from './labels';
import { WIDGETS } from './widgets';

/** 描く部品 1 つ: 部品か、使えない理由。 */
type Cell =
  | {
      key: string;
      component: Type<unknown>;
      inputs: Record<string, unknown>;
      problem: null;
    }
  | { key: string; problem: string; name: string };

/**
 * ダッシュボードを描く: まとまりごとに見出しと部品を、幅に合わせて並べる。描くのは DashboardSpec だけで、
 * どこから来たか（変数の一覧から自動で作った、ファイルから読んだ）は問わない。部品の種類が変数に合わなければ
 * （checkItem）、その場所に理由を出し、ほかの部品は描く。畳んだまとまり（collapsed）は、開くまで部品を作らない
 * （値も受け取らない）。部品は、見えている間だけ値を watch する。
 */
@Component({
  selector: 'app-dashboard-view',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [NgComponentOutlet],
  template: `
    @for (section of sections(); track section.key) {
      <section class="section" [attr.aria-labelledby]="section.id + '-title'">
        @if (section.collapsible) {
          <h2 [id]="section.id + '-title'" data-testid="dashboard-section">
            <button
              type="button"
              class="toggle"
              data-testid="dashboard-toggle"
              [attr.aria-expanded]="section.open"
              [attr.aria-controls]="section.id + '-items'"
              (click)="toggle(section.title)"
            >
              <span class="mark" aria-hidden="true">{{ section.open ? '▾' : '▸' }}</span>
              {{ section.title }}
              <span class="count" i18n>（{{ section.cells.length }} 個）</span>
            </button>
          </h2>
        } @else {
          <h2 [id]="section.id + '-title'" data-testid="dashboard-section">{{ section.title }}</h2>
        }
        @if (section.open) {
          <div class="grid" [id]="section.id + '-items'">
            @for (cell of section.cells; track cell.key) {
              @if (cell.problem === null) {
                <ng-container
                  [ngComponentOutlet]="cell.component"
                  [ngComponentOutletInputs]="cell.inputs"
                />
              } @else {
                <p class="problem" data-testid="widget-problem">
                  <span class="name">{{ cell.name }}</span>
                  <span><span class="mark" aria-hidden="true">⚠</span>{{ cell.problem }}</span>
                </p>
              }
            }
          </div>
        }
      </section>
    }
  `,
  styles: `
    :host {
      display: grid;
      gap: var(--app-space-4);
    }

    h2 {
      font: var(--mat-sys-title-large);
      margin: 0 0 var(--app-space-2);
      overflow-wrap: anywhere;
    }

    .toggle {
      font: inherit;
      color: inherit;
      background: none;
      border: none;
      padding: var(--app-space-1) 0;
      cursor: pointer;
      text-align: start;
      overflow-wrap: anywhere;
    }

    .toggle:focus-visible {
      outline: 2px solid var(--mat-sys-primary);
      outline-offset: 2px;
    }

    .count {
      font: var(--mat-sys-body-medium);
      color: var(--mat-sys-on-surface-variant);
    }

    .mark {
      margin-inline-end: var(--app-space-1);
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(min(100%, 16rem), 1fr));
      gap: var(--app-space-3);
    }

    .problem {
      display: grid;
      gap: var(--app-space-1);
      margin: 0;
      padding: var(--app-space-3);
      border: 1px dashed var(--mat-sys-error);
      border-radius: var(--mat-sys-corner-medium);
      color: var(--mat-sys-error);
      overflow-wrap: anywhere;
    }

    .problem .name {
      font: var(--mat-sys-title-small);
      color: var(--mat-sys-on-surface);
    }
  `,
})
export class DashboardView {
  private readonly variables = inject(EcuVariables);
  readonly spec = input.required<DashboardSpec>();
  /** 畳んだまとまりも開いて出す（絞り込みの最中: 当たった部品を隠さない）。 */
  readonly expandAll = input(false);

  /** 利用者が開いた、畳んであったまとまり（見出し）。 */
  private readonly flipped = signal<ReadonlySet<string>>(new Set());

  /** 描くもの。部品の input は spec か一覧が変わったときだけ作り直す（同じ変数の部品は、そのまま残る）。 */
  private readonly cells = computed(() => {
    // 同じ部品が 2 度あっても、key は重ならないようにする
    const seen = new Map<string, number>();
    const unique = (key: string) => {
      const count = seen.get(key) ?? 0;
      seen.set(key, count + 1);
      return count === 0 ? key : `${key}-${count}`;
    };
    return this.spec().sections.map((section, s) => ({
      // 絞り込みでまとまりの番号が変わっても、同じまとまりの部品を作り直さない
      key: unique(`section\u0000${section.title}`),
      id: `dashboard-section-${s}`,
      title: section.title,
      collapsed: section.collapsed === true,
      cells: section.items.map((item): Cell => {
        const key = unique(`${item.peer}\u0000${item.name}\u0000${item.widget}`);
        const info = this.variables.find(item);
        const problem: ItemProblem | null = checkItem(item, info);
        if (problem) {
          return {
            key,
            name: item.label ?? `${item.peer} / ${item.name}`,
            problem: itemProblemText(problem, item.widget, info?.type ?? ''),
          };
        }
        return {
          key,
          component: WIDGETS[item.widget as WidgetKind],
          inputs: {
            ref: { peer: item.peer, name: item.name },
            label: item.label ?? null,
            options: item.options ?? null,
          },
          problem: null,
        };
      }),
    }));
  });

  protected readonly sections = computed(() =>
    this.cells().map((section) => ({
      ...section,
      collapsible: section.collapsed,
      open: !section.collapsed || this.expandAll() || this.flipped().has(section.title),
    })),
  );

  protected toggle(title: string): void {
    this.flipped.update((flipped) => {
      const next = new Set(flipped);
      if (next.has(title)) next.delete(title);
      else next.add(title);
      return next;
    });
  }
}
