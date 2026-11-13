import {
  ChangeDetectionStrategy,
  Component,
  computed,
  inject,
  linkedSignal,
  resource,
  signal,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCardModule } from '@angular/material/card';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { RouterLink } from '@angular/router';
import { AuthToken } from '../core/auth/auth-token';
import { EcuVariables } from '../core/session/ecu-variables';
import { ServerConnection } from '../core/session/server-connection';
import { ServerInfo } from '../core/session/server-info';
import { SimulationTime } from '../core/session/simulation-time';
import { DashboardResponse, errorMessage } from '@vehicle-sil/gui-client';
import {
  BuiltinLookup,
  autoDashboard,
  countItems,
  fileDashboard,
  filterDashboard,
} from '../widgets/dashboard-spec';
import { DashboardView } from '../widgets/dashboard-view';
import { monitoringTitle } from '../widgets/labels';

/** 画面の上に出す、変数の機能の様子。english: 添える英語の文（サーバーの理由）。 */
interface Notice {
  text: string;
  english: string | null;
  tone: 'info' | 'warn';
}

/**
 * 変数の画面: 構成ファイルの隣にダッシュボードのファイル（`<名前>.dashboard.yaml`）があれば、そのとおりに並べる。
 * なければ（読めなければ）、いまの実行の ECU の変数を、ECU ごとに既定の部品で並べる（自動のダッシュボード）。
 * ECU が自分で足す監視の変数は、ECU ごとの畳んだまとまりにする（絞り込みで当たれば開く）。名前で絞り込める。
 * 部品は見えている間だけ値を受け取るので、変数が多くても重くならない。
 */
@Component({
  selector: 'app-variables-page',
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    DashboardView,
    MatButtonModule,
    MatCardModule,
    MatFormFieldModule,
    MatInputModule,
    RouterLink,
  ],
  template: `
    <h1 i18n>変数</h1>

    @if (!auth.token()) {
      <mat-card appearance="outlined" data-testid="no-token">
        <mat-card-content>
          <p i18n>
            この画面は、GUI サーバーが起動したときに表示した URL（<code>#token=</code>
            の付いたもの）から開いてください。
          </p>
        </mat-card-content>
      </mat-card>
    } @else {
      <!-- 様子の変わり目を読み上げる領域。中身が変わる前から置いておく -->
      <p
        class="notice"
        role="status"
        data-testid="variables-notice"
        [attr.data-tone]="notice()?.tone"
        [class.shown]="notice() !== null"
      >
        @if (notice(); as n) {
          @if (n.tone === 'warn') {
            <span class="mark" aria-hidden="true">⚠</span>
          }
          {{ n.text }}
          @if (n.english) {
            <span lang="en">{{ n.english }}</span>
          }
        }
      </p>

      @if (configFile()) {
        <div class="file" data-testid="dashboard-file">
          <p class="source">
            @if (fileState(); as state) {
              @switch (state.kind) {
                @case ('used') {
                  <ng-container i18n
                    >ダッシュボードのファイル
                    <code>{{ state.path }}</code> のとおりに並べています。</ng-container
                  >
                  @if (state.modules.length > 0) {
                    <span data-testid="dashboard-modules">
                      <ng-container i18n>読み込んだパネルのモジュール:</ng-container>
                      @for (module of state.modules; track $index) {
                        <code>{{ module }}</code>
                      }
                    </span>
                  }
                }
                @case ('missing') {
                  <ng-container i18n
                    >ダッシュボードのファイル
                    <code>{{ state.path }}</code>
                    がないので、変数を自動で並べています。</ng-container
                  >
                }
                @case ('errors') {
                  <ng-container i18n
                    >ダッシュボードのファイル
                    <code>{{ state.path }}</code>
                    に誤りがあるので、変数を自動で並べています。直したら「読み直す」を押してください。</ng-container
                  >
                }
                @case ('failed') {
                  <ng-container i18n
                    >ダッシュボードのファイルを読めないので、変数を自動で並べています:</ng-container
                  >
                  <span lang="en">{{ state.message }}</span>
                }
              }
            }
          </p>
          <button
            mat-stroked-button
            type="button"
            data-testid="dashboard-reload"
            [disabled]="dashboardFile.isLoading()"
            (click)="reload()"
            i18n
          >
            読み直す
          </button>
        </div>
        @if (fileErrors().length > 0) {
          <ul class="errors" data-testid="dashboard-errors">
            @for (e of fileErrors(); track $index) {
              <li>
                <span class="mark" aria-hidden="true">⚠</span>
                <ng-container i18n>{{ e.line }} 行 {{ e.column }} 桁</ng-container>
                @if (e.at) {
                  <code>{{ e.at }}</code>
                }
                <span lang="en">{{ e.message }}</span>
              </li>
            }
          </ul>
        }
        @if (dashboard().title; as title) {
          <p class="title" data-testid="dashboard-title">{{ title }}</p>
        }
      }

      @if (total() > 0) {
        <div class="tools">
          <mat-form-field class="filter" subscriptSizing="dynamic">
            <mat-label i18n>名前で絞り込む</mat-label>
            <input
              #filter
              matInput
              type="search"
              data-testid="variables-filter"
              [attr.autocomplete]="'off'"
              [value]="query()"
              (input)="query.set(filter.value)"
            />
          </mat-form-field>
          <p class="count" data-testid="variables-count">
            @if (query().trim()) {
              <ng-container i18n>{{ total() }} 個中 {{ shown() }} 個</ng-container>
            } @else {
              <ng-container i18n>{{ total() }} 個の変数</ng-container>
            }
          </p>
        </div>
        @if (omitted() > 0) {
          <p class="hint" data-testid="variables-omitted" i18n>
            サーバーの上限を超えた {{ omitted() }} 個の変数は出していません。
          </p>
        }
        @if (shown() === 0) {
          <p class="hint" data-testid="variables-none-match" i18n>名前に合う変数はありません。</p>
        }
        <app-dashboard-view [spec]="filtered()" [expandAll]="query().trim() !== ''" />
      } @else if (variables.status() === 'ready' || variables.status() === 'ended') {
        <p class="hint" data-testid="variables-empty" i18n>
          この実行の ECU は、変数を宣言していません。
        </p>
      } @else if (variables.session() === null) {
        <p class="hint" data-testid="variables-no-run" i18n>
          <a routerLink="/">「実行」の画面</a>で実行を始めると、ECU の変数がここに並びます。
        </p>
      }
    }
  `,
  styles: `
    :host {
      display: block;
    }

    h1 {
      font: var(--mat-sys-headline-small);
      margin-block: 0 var(--app-space-3);
    }

    .notice {
      margin: 0;
      padding: var(--app-space-2) var(--app-space-3);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container);
      overflow-wrap: anywhere;
    }

    .notice:not(.shown) {
      position: absolute;
      inline-size: 1px;
      block-size: 1px;
      overflow: hidden;
      clip-path: inset(50%);
      padding: 0;
    }

    .notice[data-tone='warn'] {
      background: var(--mat-sys-error-container);
      color: var(--mat-sys-on-error-container);
    }

    .notice.shown {
      margin-block-end: var(--app-space-3);
    }

    .mark {
      margin-inline-end: var(--app-space-1);
    }

    .file {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-2) var(--app-space-3);
      align-items: center;
      margin-block-end: var(--app-space-2);
    }

    .source code {
      margin-inline: var(--app-space-1);
    }

    .source {
      flex: 1 1 20rem;
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
      overflow-wrap: anywhere;
    }

    .errors {
      margin: 0 0 var(--app-space-3);
      padding: var(--app-space-2) var(--app-space-3);
      list-style: none;
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-error-container);
      color: var(--mat-sys-on-error-container);
      overflow-wrap: anywhere;
    }

    .errors li + li {
      margin-block-start: var(--app-space-1);
    }

    .errors code,
    .errors span[lang] {
      margin-inline-start: var(--app-space-1);
    }

    .title {
      font: var(--mat-sys-title-large);
      margin: 0 0 var(--app-space-3);
      overflow-wrap: anywhere;
    }

    .tools {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-2) var(--app-space-3);
      align-items: center;
      margin-block-end: var(--app-space-3);
    }

    .filter {
      flex: 1 1 16rem;
      max-inline-size: 32rem;
      min-inline-size: 0;
    }

    .count,
    .hint {
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
    }

    .hint {
      margin-block-end: var(--app-space-3);
    }
  `,
})
export class VariablesPage {
  protected readonly auth = inject(AuthToken);
  protected readonly variables = inject(EcuVariables);
  private readonly server = inject(ServerConnection);
  private readonly info = inject(ServerInfo);
  private readonly time = inject(SimulationTime);

  /** 絞り込みの語。 */
  protected readonly query = signal('');
  /** ダッシュボードのファイルを探す構成ファイル: いまの（最後の）実行のもの、なければ起動のときに渡したもの。 */
  protected readonly configFile = computed(
    () => this.server.session()?.file ?? this.info.initialFile() ?? null,
  );
  /** いまの（最後の）実行の番号（要約が変わるたびに読み直さないよう、番号だけ）。 */
  private readonly sessionId = computed(() => this.server.session()?.id ?? null);
  private readonly reloads = signal(0);
  /** ダッシュボードのファイル（構成ファイルが変わったとき、実行が替わったとき、「読み直す」で読み直す）。 */
  protected readonly dashboardFile = resource({
    params: () => {
      const file = this.configFile();
      if (!this.auth.token() || !file) return undefined;
      return { file, session: this.sessionId(), reloads: this.reloads() };
    },
    loader: ({ params }) => this.info.dashboard(params.file),
  });
  /** 最後に読み終えた結果（読み直している間は前の結果のまま: 部品を作り直さない）。 */
  private readonly loaded = linkedSignal<
    { status: string; value: DashboardResponse | undefined; error: unknown },
    { value: DashboardResponse | null; error: unknown } | null
  >({
    source: () => ({
      status: this.dashboardFile.status(),
      value: this.dashboardFile.hasValue() ? this.dashboardFile.value() : undefined,
      error: this.dashboardFile.error(),
    }),
    computation: (now, previous) =>
      now.status === 'loading' || now.status === 'reloading'
        ? (previous?.value ?? null)
        : { value: now.value ?? null, error: now.error ?? null },
  });
  /** ファイルの様子（まだ読んでいなければ null）。 */
  protected readonly fileState = computed<
    | { kind: 'used'; path: string; modules: string[] }
    | { kind: 'missing' | 'errors'; path: string }
    | { kind: 'failed'; message: string }
    | null
  >(() => {
    const loaded = this.loaded();
    if (loaded?.error) return { kind: 'failed', message: errorMessage(loaded.error) };
    const response = loaded?.value;
    if (!response) return null;
    if (!response.found) return { kind: 'missing', path: response.path };
    if (!response.dashboard) return { kind: 'errors', path: response.path };
    // パネルは画面の中でトークンを持って動く: どのモジュールを読み込んだかを見せる
    return {
      kind: 'used',
      path: response.path,
      modules: response.dashboard.panels.map((p) => p.module),
    };
  });
  protected readonly fileErrors = computed(() => this.loaded()?.value?.errors ?? []);
  /** 実行の照合の結果（要約が変わっても、照合の結果が同じなら同じもの）。 */
  private readonly check = computed(() => this.server.session()?.check ?? null);
  /** 変数が vehicle-sil の付けたものか（照合の結果の builtin）。監視の変数の見分けに使う。 */
  private readonly builtin = computed<BuiltinLookup>(() => {
    const table = new Map<string, boolean>();
    for (const participant of this.check()?.participants ?? []) {
      if (participant.name === null) continue;
      for (const variable of participant.variables ?? []) {
        if (variable.builtin !== null) {
          table.set(`${participant.name}\u0000${variable.name}`, variable.builtin);
        }
      }
    }
    return (peer, name) => table.get(`${peer}\u0000${name}`) ?? null;
  });
  /** 描くダッシュボード: ファイルがあればそのとおり、なければ変数の一覧から自動で（一覧が変わったときだけ作り直す）。 */
  protected readonly dashboard = computed(() => {
    const file = this.loaded()?.value?.dashboard;
    return file
      ? fileDashboard(file, this.variables.peers(), monitoringTitle, this.builtin())
      : autoDashboard(this.variables.peers(), monitoringTitle, this.builtin());
  });
  protected readonly filtered = computed(() => filterDashboard(this.dashboard(), this.query()));
  protected readonly total = computed(() => countItems(this.dashboard()));
  protected readonly shown = computed(() => countItems(this.filtered()));
  protected readonly omitted = computed(() => this.variables.omitted());

  protected reload(): void {
    this.reloads.update((n) => n + 1);
  }

  protected readonly notice = computed<Notice | null>(() => {
    if (this.server.connectionState() === 'closed') {
      return {
        text: $localize`サーバーとの接続が切れています。値は古いかもしれず、書けません。`,
        english: null,
        tone: 'warn',
      };
    }
    const status = this.variables.status();
    const reason = this.variables.reason();
    const app = this.info.variables();
    const session = this.server.session();
    const scenario = !!session?.scenario && session.launcher.running;
    if (scenario && (status === 'off' || status === 'failed')) {
      return {
        text: $localize`シナリオの実行中は、変数を見られません:`,
        english: reason,
        tone: 'warn',
      };
    }
    switch (status) {
      case 'ready':
        if (scenario) {
          return {
            text: $localize`シナリオの実行中です。値は見られますが、書くことはできません（シナリオの結果を変えないため）。`,
            english: null,
            tone: 'info',
          };
        }
        if (this.time.beforeStart()) {
          return {
            text: $localize`最初のステップの前で止まっています。ここで書いた値（パラメーターを含む）は、時刻 0 のステップで反映されます。再開は「実行」の画面で行います。`,
            english: null,
            tone: 'info',
          };
        }
        if (this.time.paused()) {
          return {
            text: $localize`一時停止しています。値は止まっている時刻のものです。`,
            english: null,
            tone: 'info',
          };
        }
        return null;
      case 'ended':
        return {
          text: $localize`実行は終わりました。最後に読めた値を出しています。書くことはできません。`,
          english: reason,
          tone: 'info',
        };
      case 'connecting':
        return {
          text: $localize`ランチャーにつないでいます。`,
          english: reason,
          tone: 'info',
        };
      case 'failed':
        return {
          text: $localize`ランチャーにつながらないので、変数を読み書きできません:`,
          english: reason,
          tone: 'warn',
        };
      default:
        // 実行がない、またはこの実行に変数がない: 始めても使えないなら、その理由を出す
        if (app && !app.available) {
          return {
            text: $localize`このランチャーか GUI サーバーでは、ECU の変数を読み書きできません:`,
            english: app.reason,
            tone: 'warn',
          };
        }
        if (this.variables.session() !== null && status === 'off') {
          return {
            text: $localize`この実行では、ECU の変数を読み書きできません:`,
            english: reason,
            tone: 'warn',
          };
        }
        return null;
    }
  });
}
