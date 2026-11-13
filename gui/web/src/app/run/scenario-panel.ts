import { Component, computed, input } from '@angular/core';
import { ScenarioStep, ScenarioView } from '@vehicle-sil/gui-client';
import { scenarioOutcomeLabel, stepStateLabel } from './labels';

/**
 * シナリオの経過: 全体の合否と、手順の表（行、何をするか、状態、仮想時刻、失敗の理由）。手順は、ランチャーの
 * `scenario_step` の事象で進み、結果が出たら結果の手順（動かなかった手順を含む）になる。手順の文と理由は、
 * ランチャーの英語のまま出す。
 */
@Component({
  selector: 'app-scenario-panel',
  template: `
    @let s = scenario();
    <p
      class="outcome"
      [attr.data-tone]="tone()"
      data-testid="scenario-outcome"
      [title]="s.status ?? ''"
    >
      <span class="mark" aria-hidden="true">{{ mark() }}</span>
      {{ outcome() }}
    </p>
    <p class="file">
      <code data-testid="scenario-file">{{ s.file }}</code>
      @if (s.name) {
        <span lang="en">（{{ s.name }}）</span>
      }
    </p>
    <div class="table" role="region" [attr.aria-label]="tableLabel" tabindex="0">
      <table data-testid="scenario-steps">
        <thead>
          <tr>
            <th scope="col" i18n>行</th>
            <th scope="col" i18n>手順</th>
            <th scope="col" i18n>状態</th>
            <th scope="col" i18n>仮想時刻</th>
          </tr>
        </thead>
        <tbody>
          @for (step of s.steps; track step.index) {
            <tr [attr.data-status]="step.status" data-testid="scenario-step">
              <td class="line">{{ step.line ?? '—' }}</td>
              <td class="text">
                <span lang="en">{{ step.text }}</span>
                @if (step.message) {
                  <span class="reason">
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
    .outcome {
      margin: 0 0 var(--app-space-1);
      font: var(--mat-sys-title-medium);
    }
    .outcome[data-tone='bad'],
    tr[data-status='failed'] .state {
      color: var(--mat-sys-error);
    }
    .outcome[data-tone='good'] {
      color: var(--mat-sys-primary);
    }
    .file {
      margin: 0 0 var(--app-space-2);
      overflow-wrap: anywhere;
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
    .state {
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
  readonly scenario = input.required<ScenarioView>();
  /** シナリオがまだ動いているか（ランチャーが動いていて、結果がまだ）。 */
  readonly running = input(false);

  protected readonly tableLabel = $localize`シナリオの手順`;

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
