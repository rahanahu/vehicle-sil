import { Component } from '@angular/core';
import { RouterLink } from '@angular/router';

@Component({
  selector: 'app-not-found',
  imports: [RouterLink],
  template: `
    <h1 i18n>画面が見つからない</h1>
    <p i18n>このアドレスの画面はありません。リンクが古いか、打ち間違いかもしれません。</p>
    <p><a routerLink="/" i18n>最初の画面へ</a></p>
  `,
  styles: `
    h1 {
      font: var(--mat-sys-headline-small);
      margin-block: 0 var(--app-space-3);
    }
  `,
})
export class NotFound {}
