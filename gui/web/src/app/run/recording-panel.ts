import { Component, DOCUMENT, computed, inject, input, resource, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { errorMessage } from '@vehicle-sil/gui-client';
import { AuthToken } from '../core/auth/auth-token';
import { EcuVariables } from '../core/session/ecu-variables';
import { ServerInfo } from '../core/session/server-info';

/**
 * この実行の操作をシナリオとして保存する: GUI サーバーが覚えている、反映された書き込み（`GET api/recording`）を
 * シナリオの YAML にして、ブラウザの保存（ダウンロード）として渡す。置き場所は利用者が決める（サーバーはファイルを
 * 書かない）。何件入るかは、書き込みが確定するたびに読み直して出す。
 */
@Component({
  selector: 'app-recording-panel',
  imports: [MatButtonModule],
  template: `
    @if (recording.hasValue() && recording.value(); as r) {
      @if (r.yaml !== null) {
        <p data-testid="recording-count" i18n>
          反映された書き込み {{ r.writes }} 件を、{{ r.steps }} 個の手順（set）にして保存します。
        </p>
        @if (r.omitted > 0) {
          <p class="warn" role="status" i18n>
            多すぎるので、後の {{ r.omitted }} 件は入りません（最初の {{ r.writes }} 件だけです）。
          </p>
        }
        <button
          matButton="tonal"
          type="button"
          (click)="save(r.yaml, r.file, r.session_id)"
          data-testid="recording-save"
          i18n
        >
          この実行の操作をシナリオとして保存
        </button>
        <p class="hint" i18n>
          保存したファイルを構成ファイルの隣の scenarios
          に置くと、「シナリオ」で選んで回せます。確かめる手順（expect、wait_until）はエディターで足してください。一時停止やステップ送りは入りません。
        </p>
      } @else {
        <p class="hint" data-testid="recording-empty">
          @if (r.available) {
            <ng-container i18n
              >まだ保存するものがありません。変数を書いて、反映されると数えます。</ng-container
            >
          } @else {
            <ng-container i18n>この実行の操作は、シナリオとして保存できません:</ng-container>
            <span lang="en">{{ r.reason }}</span>
          }
        </p>
      }
    } @else if (recording.error(); as error) {
      <p class="warn" role="alert">
        <ng-container i18n>保存するものを読めませんでした:</ng-container>
        <span lang="en">{{ message(error) }}</span>
      </p>
    }
    @if (saved()) {
      <p class="hint" role="status" data-testid="recording-saved" i18n>
        保存のためにブラウザへ渡しました: {{ saved() }}
      </p>
    }
  `,
  styles: `
    :host {
      display: block;
    }
    p {
      margin: 0 0 var(--app-space-2);
    }
    .hint {
      color: var(--mat-sys-on-surface-variant);
    }
    .warn {
      color: var(--mat-sys-error);
    }
  `,
})
export class RecordingPanel {
  private readonly info = inject(ServerInfo);
  private readonly auth = inject(AuthToken);
  private readonly variables = inject(EcuVariables);
  private readonly document = inject(DOCUMENT);

  /** 実行の番号と、ランチャーが動いているか（終わったら読み直す）。 */
  readonly session = input.required<number>();
  readonly running = input(false);

  /** この実行の、確定した操作（確定するたびに読み直す）。 */
  private readonly settled = computed(() =>
    this.variables
      .operations()
      .filter((op) => op.session === this.session() && op.done)
      .map((op) => op.seq)
      .join(','),
  );

  protected readonly recording = resource({
    params: () =>
      this.auth.token()
        ? { session: this.session(), running: this.running(), settled: this.settled() }
        : undefined,
    loader: () => this.info.recording(),
  });

  protected readonly saved = signal<string | null>(null);

  protected message(error: unknown): string {
    return errorMessage(error);
  }

  /** YAML をファイルとしてブラウザに渡す（名前は構成ファイルと実行の番号から）。 */
  protected save(yaml: string, file: string, session: number): void {
    const stem = (file.split('/').pop() ?? 'simulation').replace(/\.ya?ml$/i, '');
    const name = `${stem}-run${session}.yaml`;
    const url = URL.createObjectURL(new Blob([yaml], { type: 'application/yaml' }));
    const link = this.document.createElement('a');
    link.href = url;
    link.download = name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    this.saved.set(name);
  }
}
