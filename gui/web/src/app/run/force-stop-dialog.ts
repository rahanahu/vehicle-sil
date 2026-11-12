import { Component } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatDialogModule } from '@angular/material/dialog';

/**
 * 「待たずに止める」の確かめ。既定のフォーカスは「やめる」（Enter の押し続けや連打で強制終了しない）。
 * 閉じた値は、止めるなら true。
 */
@Component({
  selector: 'app-force-stop-dialog',
  imports: [MatButtonModule, MatDialogModule],
  template: `
    <h2 mat-dialog-title i18n>待たずに止めますか</h2>
    <mat-dialog-content>
      <p i18n>
        participant の終わりを待たずに、ランチャーに強制終了を頼みます。participant
        が書き終えていない結果やログは失われることがあります。
      </p>
    </mat-dialog-content>
    <mat-dialog-actions [align]="'end'">
      <button matButton type="button" [mat-dialog-close]="false" cdkFocusInitial i18n>
        やめる
      </button>
      <button
        matButton="filled"
        type="button"
        class="danger"
        data-testid="force-stop-confirm"
        [mat-dialog-close]="true"
        i18n
      >
        待たずに止める
      </button>
    </mat-dialog-actions>
  `,
  styles: `
    .danger {
      --mat-button-filled-container-color: var(--mat-sys-error);
      --mat-button-filled-label-text-color: var(--mat-sys-on-error);
    }
  `,
})
export class ForceStopDialog {}
