import {
  Component,
  DestroyRef,
  ElementRef,
  afterNextRender,
  afterRenderEffect,
  computed,
  inject,
  linkedSignal,
  viewChild,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatListModule } from '@angular/material/list';
import { MatMenuModule } from '@angular/material/menu';
import { MatSidenav, MatSidenavContainer, MatSidenavModule } from '@angular/material/sidenav';
import { MatToolbarModule } from '@angular/material/toolbar';
import { RouterLink, RouterLinkActive, RouterOutlet } from '@angular/router';
import { LanguageChoice } from './core/i18n/language-choice';
import { LanguageCode } from './core/i18n/language';
import { BREAKPOINTS, Layout } from './core/layout/layout';
import { NAV_ITEMS } from './navigation';

/** ナビゲーションの形。compact は引き出し、medium はアイコンの帯、wide は文字つき。 */
export type NavMode = 'drawer' | 'rail' | 'full';

@Component({
  selector: 'app-root',
  imports: [
    RouterOutlet,
    RouterLink,
    RouterLinkActive,
    MatToolbarModule,
    MatSidenavModule,
    MatListModule,
    MatButtonModule,
    MatMenuModule,
  ],
  templateUrl: './app.html',
  styleUrl: './app.css',
  host: {
    '[attr.data-width]': 'widthClass()',
    // 境目を CSS へ渡す（CSS の値は BREAKPOINTS だけから決まる）
    '[style.--app-breakpoint-medium]': `'${BREAKPOINTS.medium}px'`,
    '[style.--app-breakpoint-wide]': `'${BREAKPOINTS.wide}px'`,
  },
})
export class App {
  protected readonly widthClass = inject(Layout).widthClass;
  private readonly language = inject(LanguageChoice);
  private readonly container = viewChild.required(MatSidenavContainer);
  private readonly drawer = viewChild.required(MatSidenav, { read: ElementRef });
  private readonly toggle = viewChild('navToggle', { read: ElementRef<HTMLElement> });
  private readonly languageButton = viewChild.required('languageButton', {
    read: ElementRef<HTMLElement>,
  });

  protected readonly navMode = computed<NavMode>(() => {
    switch (this.widthClass()) {
      case 'compact':
        return 'drawer';
      case 'medium':
        return 'rail';
      case 'wide':
        return 'full';
    }
  });

  /** 引き出しが開いているか。幅の区分が変わったら閉じた状態に戻す。 */
  protected readonly drawerOpen = linkedSignal({
    source: this.widthClass,
    computation: () => false,
  });

  protected readonly navOpened = computed(() => this.navMode() !== 'drawer' || this.drawerOpen());

  protected readonly navItems = NAV_ITEMS;
  protected readonly openLabel = $localize`ナビゲーションを開く`;
  protected readonly closeLabel = $localize`ナビゲーションを閉じる`;

  protected readonly languages = this.language.languages;
  protected readonly currentLanguage =
    this.languages.find((language) => language.code === this.language.current) ?? this.languages[0];
  /** 言語のメニューの名前。どの言語でも「Language」を含める（訳では、その言語の語に「Language」を添える）。 */
  protected readonly languageLabel = $localize`言語（Language）: ${this.currentLanguage.name}:language:`;

  constructor() {
    // 言語を変えて読み込み直したら、フォーカスを言語のメニューへ戻す（キーボードで続けて操作できる）
    if (this.language.take('shell') === 'language') {
      afterNextRender(() => this.languageButton().nativeElement.focus());
    }
    this.language.keep('shell', () => 'language', inject(DestroyRef));
    // rail と full ではナビゲーションの幅だけが変わる。そのときだけ中身の余白を測り直す（autosize は使わない）。
    afterRenderEffect(() => {
      this.widthClass();
      this.container().updateContentMargins();
    });
  }

  protected chooseLanguage(code: LanguageCode): void {
    this.language.choose(code);
  }

  protected toggleDrawer(): void {
    this.drawerOpen.update((open) => !open);
  }

  protected closeDrawer(): void {
    if (!this.drawerOpen()) return;
    // フォーカスが引き出しの中にあれば、開くボタンへ戻す。Material に任せると、開く動きの最中に閉じたとき
    // （戻し先をまだ覚えていない）に body へ落ちる。先に移すので、Material は戻さない。
    const drawer = this.drawer().nativeElement as HTMLElement;
    if (drawer.contains(drawer.ownerDocument.activeElement)) this.toggle()?.nativeElement.focus();
    this.drawerOpen.set(false);
  }
}
