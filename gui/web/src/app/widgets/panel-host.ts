import {
  ChangeDetectionStrategy,
  Component,
  DOCUMENT,
  ElementRef,
  Injectable,
  computed,
  effect,
  inject,
  input,
  signal,
  viewChild,
} from '@angular/core';
import { DashboardPanelModule, errorMessage } from '@vehicle-sil/gui-client';
import { GUI_CLIENT } from '../core/session/gui-client';
import { DashboardPanel } from './dashboard-spec';
import { panelProblemText } from './labels';

/** モジュールを読み込めなかった理由（ファイルに書いたモジュールの名前ごと。文に URL を入れない）。 */
export interface ModuleFailure {
  module: string;
  message: string;
}

/**
 * 持ち込みのパネルのモジュールを読み込む（同じ URL は 1 回だけ）。モジュールは GUI サーバーが同じ origin で配る
 * （CSP の `script-src 'self'` のまま）。試験は、この置き換えで偽の読み込みにする。
 */
@Injectable({ providedIn: 'root' })
export class PanelModules {
  private readonly base = inject(DOCUMENT).baseURI;
  private readonly loading = new Map<string, Promise<ModuleFailure | null>>();

  /** 全部を読み込み終えるのを待ち、読み込めなかったものを返す。 */
  async load(modules: readonly DashboardPanelModule[]): Promise<ModuleFailure[]> {
    const results = await Promise.all(modules.map((m) => this.one(m)));
    return results.filter((failure): failure is ModuleFailure => failure !== null);
  }

  private one({ module, url }: DashboardPanelModule): Promise<ModuleFailure | null> {
    let loading = this.loading.get(url);
    if (!loading) {
      const href = new URL(url, this.base).href;
      loading = this.importModule(href).then(
        () => null,
        // ブラウザの文は URL を含むことがある: 画面に出す文では、URL（起動ごとの key 入り）を名前に置き換える
        (error: unknown) => ({
          module,
          message: errorMessage(error).split(href).join(module).split(url).join(module),
        }),
      );
      this.loading.set(url, loading);
    }
    return loading;
  }

  protected importModule(href: string): Promise<unknown> {
    return import(/* @vite-ignore */ href);
  }
}

/**
 * 持ち込みのパネル 1 つを置く枠: モジュールを読み込み終えたら custom element を作り、`client`（ライブラリの
 * GuiClient）、`options`、`variables` をプロパティで渡してから、ページに入れる。読み込みや定義に失敗したら、
 * その場所に理由を出す（ほかの部品は描く）。パネルの中身はパネルの責任（自分の PC に置いたコードとして信頼する）。
 */
@Component({
  selector: 'app-panel-host',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section
      class="frame"
      data-testid="dashboard-panel"
      [attr.data-panel]="panel().panel"
      [attr.aria-label]="title()"
    >
      <header class="name">{{ title() }}</header>
      <div #slot class="slot"></div>
      @if (problem(); as text) {
        <p class="problem" data-testid="panel-problem">
          <span class="mark" aria-hidden="true">⚠</span>{{ text }}
        </p>
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

    .name {
      font: var(--mat-sys-title-small);
      overflow-wrap: anywhere;
    }

    .slot {
      min-inline-size: 0;
      overflow-wrap: anywhere;
    }

    .problem {
      margin: 0;
      color: var(--mat-sys-error);
      overflow-wrap: anywhere;
    }

    .mark {
      margin-inline-end: var(--app-space-1);
    }
  `,
})
export class PanelHost {
  private readonly client = inject(GUI_CLIENT);
  private readonly loader = inject(PanelModules);
  private readonly document = inject(DOCUMENT);
  readonly panel = input.required<DashboardPanel>();
  /** 読み込むモジュール（DashboardSpec の modules）。 */
  readonly modules = input<readonly DashboardPanelModule[]>([]);

  private readonly slot = viewChild<ElementRef<HTMLElement>>('slot');
  protected readonly problem = signal<string | null>(null);
  protected readonly title = computed(() => this.panel().label ?? this.panel().panel);
  /** 作る要素と読み込むモジュール（中身が同じなら、ダッシュボードを作り直しても要素を作り直さない）。 */
  private readonly wanted = computed(() => ({ panel: this.panel(), modules: this.modules() }), {
    equal: (a, b) => JSON.stringify(a) === JSON.stringify(b),
  });

  constructor() {
    effect((onCleanup) => {
      const slot = this.slot()?.nativeElement;
      const { panel, modules } = this.wanted();
      if (!slot) return;
      let gone = false;
      onCleanup(() => {
        gone = true;
        slot.replaceChildren(); // パネルの disconnectedCallback が watch をやめる
      });
      this.problem.set(null);
      void this.loader.load(modules).then((failures) => {
        if (gone) return;
        const registry = this.document.defaultView?.customElements;
        if (!registry?.get(panel.panel)) {
          this.problem.set(panelProblemText({ kind: 'not-defined', name: panel.panel, failures }));
          return;
        }
        try {
          const element = this.document.createElement(panel.panel);
          Object.assign(element, {
            client: this.client,
            options: { ...(panel.options ?? {}) },
            variables: (panel.variables ?? []).map((v) => ({ peer: v.peer, name: v.name })),
          });
          slot.replaceChildren(element);
          // 定義はあるが、読み込めなかったモジュールがある（前に読み込んだ同じ名前の定義で描いている）
          if (failures.length > 0) {
            this.problem.set(
              panelProblemText({ kind: 'load-failed', name: panel.panel, failures }),
            );
          }
        } catch (error) {
          this.problem.set(
            panelProblemText({ kind: 'failed', name: panel.panel, message: errorMessage(error) }),
          );
        }
      });
    });
  }
}
