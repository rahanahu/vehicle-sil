import { Component, computed, inject, input, output, viewChild } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import {
  CheckScenarioParam,
  ScenarioCallFrame,
  ScenarioStep,
  ScenarioView,
} from '@vehicle-sil/gui-client';
import { LanguageChoice } from '../core/i18n/language-choice';
import { paramProvenanceLabel, scenarioOutcomeLabel, stepStateLabel } from './labels';
import { formatParamValue, normalizeProvenance } from './scenario-param-model';
import { TimelineView } from './timeline-view';

/**
 * サーバーが配る時間の軸のページ（gui/timeline）で、実行の時間の軸のファイルを開くアドレス。言語は画面と同じ。
 * トークンはアドレスに入れない: 新しいタブ（rel="opener"）は、この画面のタブの sessionStorage の写しを持ち、
 * ページはそこからトークンを読む（docs/gui.md の「シナリオの実行の時間の軸」）。
 */
export const timelinePageUrl = (timelineUrl: string, language: string): string =>
  `timeline/?src=${encodeURIComponent(`/${timelineUrl}`)}&lang=${encodeURIComponent(language)}`;

/** テンプレート呼び出しフレーム 1 段分の表示テキスト。 */
export function formatCallFrame(frame: ScenarioCallFrame): string {
  const loc =
    frame.file && frame.line !== null
      ? `${frame.file}:${frame.line}`
      : (frame.file ?? (frame.line !== null ? String(frame.line) : '?'));
  const head = frame.name ? `${loc} (${frame.name})` : loc;
  const entries = Object.entries(frame.params);
  if (!entries.length) return head;
  return `${head} [${entries.map(([k, v]) => `${k}=${formatParamValue(v)}`).join(', ')}]`;
}

/**
 * シナリオの経過: 全体の合否と、手順の表（行、何をするか、状態、仮想時刻、失敗の理由）。手順は、ランチャーの
 * `scenario_step` の事象で進み、結果が出たら結果の手順（動かなかった手順を含む）になる。手順の文と理由は、
 * ランチャーの英語のまま出す。実行が時間の軸のファイルを残したら（終わった後）、それを開くリンクと、この画面の中に
 * 出す部品（TimelineView）を出す。失敗終了時はタイムラインを自動展開し、失敗手順へフォーカスする。
 */
@Component({
  selector: 'app-scenario-panel',
  imports: [MatButtonModule, TimelineView],
  template: `
    @let s = scenario();
    <div class="top-row">
      <p
        class="outcome"
        [attr.data-tone]="tone()"
        data-testid="scenario-outcome"
        [title]="s.status ?? ''"
      >
        <span class="mark" aria-hidden="true">{{ mark() }}</span>
        {{ outcome() }}
      </p>
      <div class="top-actions">
        @if (timelineHref(); as href) {
          <a
            class="timeline-link"
            [href]="href"
            target="_blank"
            rel="opener"
            data-testid="scenario-timeline"
            i18n-title
            title="手順と値を時間の軸の図で見る（新しいタブ）"
            i18n
            >時間の軸</a
          >
        }
        @if (rerunAllowed() !== null && !running() && s.status !== null) {
          <button
            matButton="tonal"
            type="button"
            class="rerun"
            data-testid="scenario-rerun"
            [disabled]="!rerunAllowed()"
            (click)="rerun.emit()"
            i18n
          >
            同じ条件で再実行
          </button>
        }
      </div>
    </div>
    @if (handoverBadge(); as notice) {
      <p class="handover" role="status" data-testid="scenario-handover">
        <span class="mark" aria-hidden="true">‖</span>
        {{ notice }}
      </p>
    }
    @if (s.timelineUrl) {
      <app-timeline-view
        #timeline
        [timelineUrl]="s.timelineUrl"
        [autoExpand]="s.status === 'failed'"
        [initialStep]="failedStepIndex()"
      />
    }
    <p class="file">
      <code data-testid="scenario-file">{{ s.file }}</code>
      @if (s.name) {
        <span lang="en">（{{ s.name }}）</span>
      }
      @if (s.case) {
        <span class="case" lang="en" data-testid="scenario-case">[{{ s.case }}]</span>
      }
    </p>
    @if (s.params && s.params.length > 0) {
      <dl class="applied-params" data-testid="scenario-params">
        @for (param of s.params; track param.name) {
          <div class="param-chip">
            <dt lang="en">{{ param.name }}</dt>
            <dd lang="en">
              {{ formatValue(param) }}
              <span class="param-from">{{ paramSource(param) }}</span>
            </dd>
          </div>
        }
      </dl>
    }
    <div class="table" role="region" [attr.aria-label]="tableLabel" tabindex="0">
      <table data-testid="scenario-steps">
        <thead>
          <tr>
            <th scope="col" i18n>行</th>
            <th scope="col" i18n>手順</th>
            <th scope="col" i18n>状態</th>
            <th scope="col" i18n>仮想時刻</th>
            @if (s.timelineUrl) {
              <th scope="col" i18n>表示</th>
            }
          </tr>
        </thead>
        <tbody>
          @for (step of s.steps; track step.index) {
            <tr [attr.data-status]="step.status" data-testid="scenario-step">
              <td class="line" lang="en">{{ stepLocation(step, s.file) }}</td>
              <td class="text">
                @if (step.name) {
                  <span class="step-name" lang="en" data-testid="scenario-step-name">{{
                    step.name
                  }}</span>
                }
                <span lang="en">{{ step.text }}</span>
                @if (step.call_stack && step.call_stack.length > 0) {
                  <details
                    class="call-stack"
                    data-testid="scenario-step-call-stack"
                    [open]="step.status === 'failed'"
                  >
                    <summary i18n>呼び出し元（{{ step.call_stack.length }} 段）</summary>
                    <ol class="call-frames" lang="en">
                      @for (frame of step.call_stack; track $index) {
                        <li>{{ formatFrame(frame) }}</li>
                      }
                    </ol>
                  </details>
                }
                @if (step.message) {
                  <span class="reason" data-testid="scenario-step-message">
                    <span class="code">{{ step.code }}</span>
                    <span lang="en">{{ step.message }}</span>
                  </span>
                }
              </td>
              <td class="state" [title]="step.status">
                <span class="mark" aria-hidden="true">{{ stepMark(step) }}</span>
                {{ stepState(step) }}
              </td>
              <td class="time">{{ timeText(step) }}</td>
              @if (s.timelineUrl) {
                <td class="action">
                  <button
                    matButton
                    type="button"
                    class="jump"
                    [attr.data-testid]="'scenario-step-jump-' + step.index"
                    (click)="jumpToStep(step.index)"
                    i18n
                  >
                    タイムラインで表示
                  </button>
                </td>
              }
            </tr>
          }
        </tbody>
      </table>
    </div>
    @if (waiting() > 0) {
      <p class="hint" i18n>
        {waiting(), plural,
          other {ほかに {{ waiting() }} 個の手順があります（まだ始まっていません）。}
        }
      </p>
    }
    @if (s.stepsOmitted > 0) {
      <p class="hint" i18n>
        {s.stepsOmitted, plural,
          other {多すぎるので、{{ s.stepsOmitted }} 個の手順を出していません。}
        }
      </p>
    }
  `,
  styles: `
    :host {
      display: block;
    }
    .top-row {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: var(--app-space-1) var(--app-space-2);
      margin-block-end: var(--app-space-1);
    }
    .top-actions {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--app-space-2);
    }
    .timeline-link {
      white-space: nowrap;
    }
    .outcome {
      margin: 0;
      font: var(--mat-sys-title-medium);
    }
    .outcome[data-tone='bad'],
    tr[data-status='failed'] .state {
      color: var(--mat-sys-error);
    }
    .outcome[data-tone='good'] {
      color: var(--mat-sys-primary);
    }
    .handover {
      margin: var(--app-space-1) 0 var(--app-space-2);
      padding: var(--app-space-2) var(--app-space-3);
      border-inline-start: 3px solid var(--mat-sys-primary);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-low);
      color: var(--mat-sys-on-surface);
      overflow-wrap: anywhere;
    }
    .handover .mark {
      margin-inline-end: var(--app-space-1);
      color: var(--mat-sys-primary);
    }
    .file,
    .timeline {
      margin: 0 0 var(--app-space-2);
      overflow-wrap: anywhere;
    }
    .case {
      margin-inline-start: var(--app-space-1);
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
    }
    .applied-params {
      display: flex;
      flex-wrap: wrap;
      gap: var(--app-space-1) var(--app-space-2);
      margin: 0 0 var(--app-space-2);
    }
    .param-chip {
      display: inline-flex;
      align-items: baseline;
      gap: var(--app-space-1);
      padding: 2px 8px;
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-low);
      font: var(--mat-sys-body-small);
    }
    .param-chip dt {
      font-family: ui-monospace, 'Cascadia Mono', Menlo, Consolas, monospace;
      color: var(--mat-sys-on-surface-variant);
    }
    .param-chip dt::after {
      content: '=';
      margin-inline-start: 2px;
    }
    .param-chip dd {
      margin: 0;
      display: inline-flex;
      align-items: baseline;
      gap: 4px;
    }
    .param-from {
      font: var(--mat-sys-label-small);
      color: var(--mat-sys-on-surface-variant);
    }
    .table {
      overflow-x: auto;
    }
    table {
      border-collapse: collapse;
      inline-size: 100%;
    }
    th,
    td {
      text-align: start;
      vertical-align: top;
      padding: var(--app-space-1) var(--app-space-2);
      border-block-end: 1px solid var(--mat-sys-outline-variant);
    }
    th {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
    }
    .text {
      overflow-wrap: anywhere;
      min-inline-size: 12em;
    }
    .step-name {
      display: block;
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
    }
    .call-stack {
      margin-block-start: var(--app-space-1);
      font: var(--mat-sys-body-small);
      color: var(--mat-sys-on-surface-variant);
    }
    .call-stack summary {
      cursor: pointer;
    }
    .call-frames {
      margin: var(--app-space-1) 0 0;
      padding-inline-start: 1.25em;
    }
    .reason {
      display: block;
      margin-block-start: var(--app-space-1);
      color: var(--mat-sys-error);
    }
    .code {
      font: var(--mat-sys-label-medium);
      margin-inline-end: var(--app-space-1);
    }
    .line,
    .time,
    .state,
    .action {
      white-space: nowrap;
    }
    tr[data-status='running'] {
      background: var(--mat-sys-surface-container-low);
    }
    .hint {
      margin: var(--app-space-2) 0 0;
      color: var(--mat-sys-on-surface-variant);
    }
  `,
})
export class ScenarioPanel {
  private readonly language = inject(LanguageChoice).current;
  private readonly timeline = viewChild<TimelineView>('timeline');

  readonly scenario = input.required<ScenarioView>();
  /** シナリオがまだ動いているか（ランチャーが動いていて、結果がまだ）。 */
  readonly running = input(false);
  /** 実行が一時停止中か。 */
  readonly paused = input(false);
  /** ランチャーが異常終了したか。 */
  readonly crashed = input(false);
  /** 同条件での再実行ボタンの可否（null のときはボタンを出さない）。 */
  readonly rerunAllowed = input<boolean | null>(null);
  /** 「同じ条件で再実行」が押された。 */
  readonly rerun = output();

  protected readonly handoverBadge = computed(() => {
    const s = this.scenario();
    if (!this.running() || this.crashed() || !s.handedOver || s.status !== 'passed') {
      return null;
    }
    const atMs = s.overAtMs;
    if (this.paused()) {
      return atMs !== null
        ? $localize`シナリオは ${atMs}:ms: ms で完了し、手動操作のため一時停止しています。`
        : $localize`シナリオが完了し、手動操作のため一時停止しています。`;
    }
    return atMs !== null
      ? $localize`シナリオは ${atMs}:ms: ms で完了し、手動操作へ引き継ぎました。`
      : $localize`シナリオが完了し、手動操作へ引き継ぎました。`;
  });

  protected readonly tableLabel = $localize`シナリオの手順`;

  /** 時間の軸のページのアドレス。実行がファイルを残していなければ null（リンクを出さない）。 */
  protected readonly timelineHref = computed(() => {
    const url = this.scenario().timelineUrl;
    return url ? timelinePageUrl(url, this.language) : null;
  });

  /** 失敗した最初の手順番号（1 始まり）。失敗していなければ null。 */
  protected readonly failedStepIndex = computed(() => {
    const s = this.scenario();
    if (s.status !== 'failed') return null;
    return s.steps.find((step) => step.status === 'failed')?.index ?? null;
  });

  protected readonly tone = computed(() => {
    const status = this.scenario().status;
    if (status === null) return 'neutral';
    return status === 'passed' ? 'good' : 'bad';
  });

  protected readonly mark = computed(
    () => ({ good: '✓', neutral: '▶', bad: '✕' })[this.tone()] ?? '■',
  );

  protected readonly outcome = computed(() => {
    const s = this.scenario();
    if (s.status !== null) return scenarioOutcomeLabel(s.status);
    const done = s.steps.filter((step) => step.status !== 'running').length;
    const total = s.stepCount ?? s.steps.length;
    return this.running()
      ? $localize`シナリオを進めています（${done}:done: / ${total}:total: 手順）`
      : $localize`シナリオの結果はありません（${done}:done: / ${total}:total: 手順）`;
  });

  /** まだ事象の来ていない手順の数（結果の前だけ）。 */
  protected readonly waiting = computed(() => {
    const s = this.scenario();
    if (s.status !== null || s.stepCount === null) return 0;
    return Math.max(0, s.stepCount - s.steps.length);
  });

  protected jumpToStep(index: number): void {
    this.timeline()?.showStep(index);
  }

  protected formatFrame(frame: ScenarioCallFrame): string {
    return formatCallFrame(frame);
  }

  protected formatValue(param: CheckScenarioParam): string {
    return formatParamValue(param.value);
  }

  protected paramSource(param: CheckScenarioParam): string {
    return paramProvenanceLabel(normalizeProvenance(param.from, false, false), param.from);
  }

  protected stepLocation(step: ScenarioStep, rootFile: string): string {
    if (step.file && step.file !== rootFile) {
      return step.line !== null ? `${step.file}:${step.line}` : step.file;
    }
    return step.line !== null ? String(step.line) : '—';
  }

  protected stepState(step: ScenarioStep): string {
    return stepStateLabel(step.status);
  }

  /** 色だけで伝えないための形。 */
  protected stepMark(step: ScenarioStep): string {
    switch (step.status) {
      case 'passed':
        return '✓';
      case 'failed':
        return '✕';
      case 'running':
        return '▶';
      default:
        return '–';
    }
  }

  /** 決めた時刻。まだ決まっていなければ、手順の時刻から。 */
  protected timeText(step: ScenarioStep): string {
    if (step.at_ms !== null) return `${step.at_ms} ms`;
    if (step.start_ms !== null) return $localize`${step.start_ms}:ms: ms から`;
    return '—';
  }
}
