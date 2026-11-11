// 通しの確認用（後で捨てる）。消し方は README.md の「仮の画面」。ルーティングとナビゲーションを確かめるための 2 つ目の画面。
import { Component } from '@angular/core';

@Component({
  selector: 'app-about-page',
  template: `
    <h1 i18n>この画面について</h1>
    <p i18n>
      vehicle-sil の画面の開発環境を確かめるための仮の画面です。本物の画面を作るときに捨てます。
    </p>
  `,
  styles: `
    h1 {
      font: var(--mat-sys-headline-small);
      margin-block: 0 var(--app-space-3);
    }
  `,
})
export class AboutPage {}
