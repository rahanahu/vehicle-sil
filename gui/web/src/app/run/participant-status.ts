import { Component, LOCALE_ID, computed, inject, input, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
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
  imports: [MatButtonModule],
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
          @if (row.command; as command) {
            <div class="manual" [attr.data-testid]="'manual-command-' + row.name">
              @if (command.environment.length > 0) {
                <div [attr.data-testid]="'manual-environment-' + row.name">
                  <span i18n>
                    起動する前に、次の環境変数を設定します（設定の書き方は、シェルや IDE
                    によって違います）:
                  </span>
                  <ul class="environment">
                    @for (variable of command.environment; track variable.name) {
                      <li [attr.data-testid]="'manual-variable-' + variable.name">
                        <span i18n>名前</span>
                        <code class="variable-name">{{ variable.name }}</code>
                        <span i18n>値</span>
                        <code class="variable-value">{{ variable.value }}</code>
                        <button
                          matButton
                          type="button"
                          (click)="copy(row.name + '/' + variable.name, variable.assignment)"
                          [attr.aria-label]="copyVariableLabel(variable.name)"
                          [title]="variable.assignment"
                        >
                          @if (copied() === row.name + '/' + variable.name) {
                            <ng-container i18n>コピーしました</ng-container>
                          } @else {
                            <ng-container i18n>名前=値 をコピー</ng-container>
                          }
                        </button>
                      </li>
                    }
                  </ul>
                </div>
              }
              @if (command.directory) {
                <span i18n>ディレクトリ</span>
                <code class="directory">{{ command.directory }}</code>
                <span i18n>で、次のコマンドを実行します:</span>
              } @else {
                <span i18n>次のコマンドを実行します:</span>
              }
              <pre class="command"><code>{{ command.line }}</code></pre>
              <button
                matButton
                type="button"
                (click)="copy(row.name, command.line)"
                [attr.aria-label]="copyLabel(row.name)"
              >
                @if (copied() === row.name) {
                  <ng-container i18n>コピーしました</ng-container>
                } @else {
                  <ng-container i18n>コピー</ng-container>
                }
              </button>
            </div>
          }
          @if (row.stays; as stays) {
            <span
              class="stays"
              [title]="stays"
              [attr.data-testid]="'manual-stays-' + row.name"
              i18n
            >
              シミュレーションが終わっても残っています。ランチャーは終わらせないので、自分で終わらせてください。
            </span>
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
    .manual {
      flex-basis: 100%;
      min-inline-size: 0;
    }
    .command {
      margin: var(--app-space-1) 0;
      padding: var(--app-space-2);
      border-radius: var(--mat-sys-corner-extra-small);
      background: var(--mat-sys-surface-container-high);
      white-space: pre-wrap;
      overflow-wrap: anywhere;
      user-select: all;
    }
    .environment {
      margin: var(--app-space-1) 0;
      padding: 0;
      list-style: none;
    }
    .environment li {
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: 0 var(--app-space-2);
    }
    .stays {
      flex-basis: 100%;
      color: var(--mat-sys-error);
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
  /** 実行が終わった（結果が出た）。参加しなかった、自分で起動する participant をそう出す。 */
  readonly ended = input(false);
  /**
   * 自分で起動した participant のうち、終わった後もシミュレーションに残っているとランチャーが知らせたもの（名前から
   * ランチャーのログの行へ）。
   */
  readonly stays = input<ReadonlyMap<string, string>>(new Map());
  /** 最後に押したコピーのボタン（copy の key）。 */
  protected readonly copied = signal<string | null>(null);

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
          command: null,
          stays: null,
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
      // 自分で起動する participant: 待っている間は、起動するコマンドを出す（ランチャーのままの文字列。テキストとして出す）
      const waiting = p.manual && p.state === 'manual' && !decided;
      return {
        name: p.name,
        label: decided
          ? participantStateLabel('failed')
          : waiting && this.ended()
            ? $localize`参加しなかった（自分で起動）`
            : p.manual && p.state === 'joined'
              ? $localize`参加した（自分で起動）`
              : participantStateLabel(p.state),
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
        command:
          waiting && !this.ended() && p.commandLine !== null
            ? {
                line: p.commandLine,
                directory: p.workingDirectory,
                // ランチャーが言う環境変数（registry がネットワークで待ち受けるとき）。設定の書き方はシェルや IDE
                // ごとに違うので、名前と値を分けて出し、コピーは多くの場所でそのまま使える 名前=値 にする
                environment: Object.entries(p.environment).map(([name, value]) => ({
                  name,
                  value,
                  assignment: `${name}=${value}`,
                })),
              }
            : null,
        stays: p.manual ? (this.stays().get(p.name) ?? null) : null,
      };
    }),
  );

  protected copyLabel(name: string): string {
    return $localize`${name}:name: を起動するコマンドをコピー`;
  }

  protected copyVariableLabel(name: string): string {
    return $localize`環境変数 ${name}:name: を 名前=値 の形でコピー`;
  }

  /** key: コマンドは participant の名前、環境変数は「participant/変数」。押したボタンだけを「コピーしました」にする。 */
  protected copy(key: string, text: string): void {
    void navigator.clipboard?.writeText(text).then(() => this.copied.set(key));
  }

  protected readonly hasUnknown = computed(() => this.rows().some((row) => row.tone === 'unknown'));
}
