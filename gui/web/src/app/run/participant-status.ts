import { Component, computed, input } from '@angular/core';
import { participantStateLabel } from './labels';
import { ParticipantView } from './run-state';

/** 実行中の participant ごとの状態（起動、参加、終了と終了コード）。 */
@Component({
  selector: 'app-participant-status',
  template: `
    <ul class="list" aria-label="participant の状態" i18n-aria-label>
      @for (row of rows(); track row.name) {
        <li class="item" [attr.data-state]="row.tone" [attr.data-testid]="'status-' + row.name">
          <span class="name">{{ row.name }}</span>
          <span class="state">
            <span class="mark" aria-hidden="true">{{ row.mark }}</span>
            {{ row.label }}
          </span>
          @if (row.exit !== null) {
            <span class="exit" i18n>終了コード {{ row.exit }}</span>
          }
          @if (row.signal) {
            <span class="exit" i18n>シグナル {{ row.signal }}</span>
          }
          @if (row.required === false) {
            <span class="optional" i18n>任意</span>
          }
        </li>
      } @empty {
        <li class="optional" i18n>まだ participant の知らせがありません。</li>
      }
    </ul>
  `,
  styles: `
    .list {
      margin: 0;
      padding: 0;
      list-style: none;
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(min(100%, 14em), 1fr));
      gap: var(--app-space-2);
    }
    .item {
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: 0 var(--app-space-2);
      padding: var(--app-space-2);
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-low);
      min-inline-size: 0;
      overflow-wrap: anywhere;
    }
    .name {
      font: var(--mat-sys-title-small);
      flex-basis: 100%;
    }
    .item[data-state='bad'] .state,
    .item[data-state='bad'] .exit {
      color: var(--mat-sys-error);
    }
    .item[data-state='good'] .state {
      color: var(--mat-sys-primary);
    }
    .exit,
    .optional {
      color: var(--mat-sys-on-surface-variant);
    }
  `,
})
export class ParticipantStatus {
  readonly participants = input.required<readonly ParticipantView[]>();

  protected readonly rows = computed(() =>
    this.participants().map((p) => {
      const failed =
        p.state === 'killed' ||
        (p.state === 'ended' && (p.exitCode ?? 0) !== 0) ||
        p.signal !== null;
      const tone = failed ? 'bad' : p.state === 'joined' ? 'good' : 'plain';
      return {
        name: p.name,
        label: participantStateLabel(p.state),
        // 色だけで伝えないための形
        mark: failed ? '✕' : p.state === 'joined' ? '●' : p.state === 'ended' ? '✓' : '○',
        tone,
        exit: p.state === 'ended' || p.state === 'killed' ? p.exitCode : null,
        signal: p.signal,
        required: p.required,
      };
    }),
  );
}
