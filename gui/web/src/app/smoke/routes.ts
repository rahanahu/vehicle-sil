// 通しの確認用（後で捨てる）。消し方は README.md の「仮の画面」。
import { Routes } from '@angular/router';
import type { NavItem } from '../navigation';

export const SMOKE_ROUTES: Routes = [
  { path: '', pathMatch: 'full', redirectTo: 'smoke' },
  {
    path: 'smoke',
    title: $localize`通しの確認 - vehicle-sil`,
    loadComponent: () => import('./smoke-page').then((m) => m.SmokePage),
  },
  {
    path: 'about',
    title: $localize`この画面について - vehicle-sil`,
    loadComponent: () => import('./about-page').then((m) => m.AboutPage),
  },
];

export const SMOKE_NAV: readonly NavItem[] = [
  {
    path: '/smoke',
    label: $localize`通しの確認`,
    icon: 'M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z',
  },
  {
    path: '/about',
    label: $localize`この画面について`,
    icon: 'M12 2a10 10 0 1 0 0 20a10 10 0 1 0 0-20zm0 2a8 8 0 1 1 0 16a8 8 0 1 1 0-16zM11 10h2v7h-2zM11 7h2v2h-2z',
  },
];
