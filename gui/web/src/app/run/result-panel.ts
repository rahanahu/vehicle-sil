import { Component, LOCALE_ID, computed, inject, input } from '@angular/core';
import { RunResult } from '../core/api';
import { FindingsList } from './findings-list';
import { interruptedByLabel, runOutcomeLabel, simulationLabel } from './labels';

/** 実行の結果: 結果と、どう終わったか、理由（所見）。 */
@Component({
  selector: 'app-result-panel',
  imports: [FindingsList],
  template: `
    <!-- ランチャーの英語の値（result、simulation、interrupted_by）は、日本語の横に並べず title に置く -->
    <p class="outcome" [attr.data-tone]="tone()" [title]="result().result" data-testid="result">
      <span class="mark" aria-hidden="true">{{ mark() }}</span>
      {{ outcome() }}
    </p>
    <dl class="facts">
      @if (result().simulation; as simulation) {
        <div>
          <dt i18n>終わり方</dt>
          <dd [title]="simulation">{{ simulationText(simulation) }}</dd>
        </div>
      }
      @if (result().interrupted_by; as by) {
        <div>
          <dt i18n>止めたもの</dt>
          <dd [title]="by">{{ interruptedBy(by) }}</dd>
        </div>
      }
      <div>
        <dt i18n>所見の数</dt>
        <dd i18n>
          誤り {{ result().errors }}、警告 {{ result().warnings }}、注記 {{ result().notes }}
        </dd>
      </div>
    </dl>
    @if (leftProcesses()) {
      <p class="failed" i18n>プロセスが残った participant: {{ leftProcesses() }}</p>
    }
    <app-findings-list [findings]="result().findings" label="実行の所見" i18n-label />
  `,
  styles: `
    .outcome {
      margin: 0 0 var(--app-space-2);
      font: var(--mat-sys-title-large);
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: var(--app-space-2);
    }
    .failed,
    .outcome[data-tone='bad'] {
      color: var(--mat-sys-error);
    }
    .outcome[data-tone='good'] {
      color: var(--mat-sys-primary);
    }
    .facts {
      display: flex;
      flex-wrap: wrap;
      gap: 0 var(--app-space-4);
      margin: 0 0 var(--app-space-3);
    }
    .facts div {
      display: flex;
      gap: var(--app-space-2);
    }
    .facts dt {
      color: var(--mat-sys-on-surface-variant);
    }
    .facts dd {
      margin: 0;
    }
  `,
})
export class ResultPanel {
  private readonly list = new Intl.ListFormat(inject(LOCALE_ID), { type: 'conjunction' });
  readonly result = input.required<RunResult>();

  /** 成功は ✓、利用者が止めた中断は ■ で色を付けない（失敗ではない）、ほかは ✕。 */
  protected readonly tone = computed(() =>
    this.result().result === 'passed'
      ? 'good'
      : this.result().result === 'interrupted'
        ? 'neutral'
        : 'bad',
  );
  protected readonly mark = computed(() => ({ good: '✓', neutral: '■', bad: '✕' })[this.tone()]);
  protected readonly outcome = computed(() => runOutcomeLabel(this.result().result));
  /** プロセスが残った participant（言語の並べ方で）。なければ空。 */
  protected readonly leftProcesses = computed(() =>
    this.list.format(
      this.result()
        .participants.filter((p) => p.left_processes)
        .map((p) => p.name),
    ),
  );

  protected simulationText(simulation: string): string {
    return simulationLabel(simulation);
  }

  protected interruptedBy(by: string): string {
    return interruptedByLabel(by);
  }
}
