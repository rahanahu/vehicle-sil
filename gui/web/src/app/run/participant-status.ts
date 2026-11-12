import { Component, LOCALE_ID, computed, inject, input } from '@angular/core';
import { ParticipantView } from '@vehicle-sil/gui-client';
import { controlLabel, failureReasonLabel, participantStateLabel } from './labels';

/** 終わりが決まった状態。ほかの状態は、ランチャーが先に終わると、その後どうなったかわからない。 */
const SETTLED: ReadonlySet<string> = new Set([
  'ended',
  'killed',
  'disabled',
  'not_started',
  'failed',
]);

/** 実行中の participant ごとの状態（起動、参加、終了と終了コード）。 */
@Component({
  selector: 'app-participant-status',
  template: `
    @if (hasUnknown()) {
      <p class="unknown-hint" data-testid="participants-unknown" i18n>
        ランチャーが先に終わったので、終わりを知らせてこなかった participant
        がどうなったかはわかりません。
      </p>
    }
    <ul class="list" aria-label="participant の状態" i18n-aria-label>
      @for (row of rows(); track row.name) {
        <li class="item" [attr.data-state]="row.tone" [attr.data-testid]="'status-' + row.name">
          <span class="name">{{ row.name }}</span>
          <span class="state" [title]="row.raw">
            <span class="mark" aria-hidden="true">{{ row.mark }}</span>
            {{ row.label }}
          </span>
          @if (row.failure) {
            <span class="exit" [title]="row.failureRaw">{{ row.failure }}</span>
          }
          @if (row.exit !== null) {
            <span class="exit" i18n>終了コード {{ row.exit }}</span>
          }
          @if (row.signal) {
            <span class="exit" i18n>シグナル {{ row.signal }}</span>
          }
          @if (row.lastKnown) {
            <span class="exit" i18n>最後の知らせ: {{ row.lastKnown }}</span>
          }
          @if (row.control; as control) {
            <span
              class="control"
              [class.trouble]="control.trouble"
              [title]="control.raw"
              [attr.data-testid]="'control-' + row.name"
            >
              @if (control.trouble) {
                <span class="mark" aria-hidden="true">⚠</span>
              }
              {{ control.text }}
            </span>
          }
          @if (row.required === false) {
            <span class="optional" i18n>任意</span>
          }
          @if (row.omitted) {
            <span class="omitted" i18n>大きすぎて省いた欄: {{ row.omitted }}</span>
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
    .item[data-state='stopped'] .state,
    .item[data-state='stopped'] .exit {
      color: var(--mat-sys-on-surface-variant);
    }
    .item[data-state='good'] .state {
      color: var(--mat-sys-primary);
    }
    .item[data-state='unknown'] .state {
      color: var(--mat-sys-on-surface-variant);
    }
    .unknown-hint {
      margin: 0 0 var(--app-space-2);
      color: var(--mat-sys-on-surface-variant);
    }
    .exit,
    .optional,
    .omitted,
    .control {
      color: var(--mat-sys-on-surface-variant);
    }
    .control.trouble {
      color: var(--mat-sys-error);
    }
    .omitted {
      flex-basis: 100%;
      font: var(--mat-sys-body-small);
    }
  `,
})
export class ParticipantStatus {
  private readonly list = new Intl.ListFormat(inject(LOCALE_ID), { type: 'conjunction' });
  readonly participants = input.required<readonly ParticipantView[]>();
  /** 利用者が止めた実行か。止めたことによるシグナルや強制終了は、失敗の色にしない。 */
  readonly interrupted = input(false);
  /**
   * ランチャーが結果を出さずに終わった。終わりを知らせてこなかった participant は、最後に届いた状態（「参加した」
   * など）のままでは誤りなので「不明」にする。
   */
  readonly launcherGone = input(false);

  protected readonly rows = computed(() =>
    this.participants().map((p) => {
      const omitted = p.omittedFields.length > 0 ? this.list.format(p.omittedFields) : null;
      // 変数を読み書きする口（ランチャーの英語の状態は title に）
      const control = p.controlLink
        ? { ...controlLabel(p.controlLink), raw: p.controlLink.state }
        : null;
      if (this.launcherGone() && !SETTLED.has(p.state ?? '')) {
        return {
          name: p.name,
          label: $localize`不明`,
          mark: '?',
          tone: 'unknown',
          exit: null,
          signal: null,
          lastKnown: p.state === null ? null : participantStateLabel(p.state),
          required: p.required,
          omitted,
          raw: p.state ?? '',
          failure: null,
          failureRaw: null,
          control,
        };
      }
      // ランチャーが失敗と決めた participant は、プロセスが終わる前でもすぐに失敗として出す（後の ended でも残す）
      const decided = p.failure !== null;
      const failed =
        decided ||
        p.state === 'killed' ||
        (p.state === 'ended' && (p.exitCode ?? 0) !== 0) ||
        (p.state === 'not_started' && p.message !== null) ||
        p.signal !== null;
      // 止めたとき（利用者が止めた、またはランチャーが止まるよう求めた）の SIGTERM などは失敗ではない。
      // 利用者が止めたのに 0 以外の終了コードで終わったものは失敗のまま
      const stopped =
        failed &&
        !decided &&
        (p.stopRequested ||
          (this.interrupted() && !(p.state === 'ended' && (p.exitCode ?? 0) !== 0)));
      const tone = stopped ? 'stopped' : failed ? 'bad' : p.state === 'joined' ? 'good' : 'plain';
      return {
        name: p.name,
        label: decided ? participantStateLabel('failed') : participantStateLabel(p.state),
        // 色だけで伝えないための形
        mark: stopped
          ? '■'
          : failed
            ? '✕'
            : p.state === 'joined'
              ? '●'
              : p.state === 'ended'
                ? '✓'
                : '○',
        tone,
        exit: p.exitCode,
        signal: p.signal,
        lastKnown: null,
        required: p.required,
        omitted,
        raw: p.state === 'not_started' && p.message ? `${p.state}: ${p.message}` : (p.state ?? ''),
        // 理由は日本語で、ランチャーの英語（reason と message）は title に
        failure: decided ? (p.failure ? failureReasonLabel(p.failure) : null) : null,
        failureRaw: decided ? [p.failure, p.message].filter(Boolean).join(': ') : null,
        control,
      };
    }),
  );

  protected readonly hasUnknown = computed(() => this.rows().some((row) => row.tone === 'unknown'));
}
