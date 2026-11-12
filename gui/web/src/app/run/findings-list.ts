import { Component, computed, input } from '@angular/core';
import { Finding, severityRank } from '@vehicle-sil/gui-client';
import { severityLabel } from './labels';

/** 所見の一覧。重い順（同じ重さは届いた順）。文はランチャーの英語のまま出す。 */
@Component({
  selector: 'app-findings-list',
  template: `
    @if (sorted().length > 0) {
      <ul class="findings" [attr.aria-label]="label()">
        @for (finding of sorted(); track $index) {
          <li class="finding" [attr.data-severity]="rank(finding)" data-testid="finding">
            <span class="severity">
              <span class="mark" aria-hidden="true">{{ mark(finding) }}</span>
              {{ severityText(finding) }}
            </span>
            <code class="code">{{ finding.code }}</code>
            @if (finding.file) {
              <span class="where"
                >{{ finding.file }}{{ finding.line !== null ? ':' + finding.line : '' }}</span
              >
            }
            <span class="message" lang="en">{{ finding.message }}</span>
            @if (finding.detail) {
              <pre class="detail" lang="en">{{ finding.detail }}</pre>
            }
          </li>
        }
      </ul>
    } @else {
      <p class="none" i18n>所見はありません。</p>
    }
  `,
  styles: `
    .findings {
      margin: 0;
      padding: 0;
      list-style: none;
      display: grid;
      gap: var(--app-space-2);
    }
    .finding {
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: var(--app-space-1) var(--app-space-2);
      padding: var(--app-space-2);
      border-inline-start: 4px solid var(--mat-sys-outline);
      background: var(--mat-sys-surface-container-low);
      overflow-wrap: anywhere;
      min-inline-size: 0;
    }
    .finding[data-severity='0'] {
      border-color: var(--mat-sys-error);
    }
    .finding[data-severity='1'] {
      border-color: var(--mat-sys-tertiary);
    }
    .severity {
      font: var(--mat-sys-label-large);
    }
    .finding[data-severity='0'] .severity {
      color: var(--mat-sys-error);
    }
    .code,
    .where {
      font: var(--mat-sys-label-medium);
      color: var(--mat-sys-on-surface-variant);
    }
    .message {
      flex-basis: 100%;
    }
    .detail {
      flex-basis: 100%;
      margin: 0;
      white-space: pre-wrap;
      font-size: 0.85em;
    }
    .none {
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
    }
  `,
})
export class FindingsList {
  readonly findings = input.required<readonly Finding[]>();
  /** 一覧の名前（読み上げ用）。 */
  readonly label = input($localize`所見`);

  protected readonly sorted = computed(() =>
    this.findings()
      .map((finding, i) => ({ finding, i }))
      .sort(
        (a, b) => severityRank(a.finding.severity) - severityRank(b.finding.severity) || a.i - b.i,
      )
      .map(({ finding }) => finding),
  );

  protected rank(finding: Finding): number {
    return severityRank(finding.severity);
  }

  protected severityText(finding: Finding): string {
    return severityLabel(finding.severity);
  }

  /** 色だけで伝えないための形（✕ 誤り、! 警告、i 注記）。 */
  protected mark(finding: Finding): string {
    return ['✕', '!', 'i'][severityRank(finding.severity)];
  }
}
