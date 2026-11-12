import { Component, LOCALE_ID, computed, inject, input } from '@angular/core';
import { RunResult } from '../core/api';
import { FindingsList } from './findings-list';
import { runOutcomeLabel, simulationLabel } from './labels';

/** 実行の結果: 結果と、どう終わったか、理由（所見）。 */
@Component({
  selector: 'app-result-panel',
  imports: [FindingsList],
  template: `
    <p class="outcome" [class.failed]="!passed()" data-testid="result">
      <span class="mark" aria-hidden="true">{{ passed() ? '✓' : '✕' }}</span>
      {{ outcome() }}
      <code class="raw">{{ result().result }}</code>
    </p>
    <dl class="facts">
      @if (result().simulation; as simulation) {
        <div>
          <dt i18n>終わり方</dt>
          <dd>{{ simulationText(simulation) }}</dd>
        </div>
      }
      @if (result().interrupted_by; as by) {
        <div>
          <dt i18n>中断</dt>
          <dd>{{ by }}</dd>
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
    .failed {
      color: var(--mat-sys-error);
    }
    .raw {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
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

  protected readonly passed = computed(() => this.result().result === 'passed');
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
}
