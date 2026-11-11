import { Routes } from '@angular/router';
import { SMOKE_ROUTES } from './smoke/routes';

export const routes: Routes = [
  ...SMOKE_ROUTES,
  {
    // レイアウトの e2e（e2e/layout.spec.ts）が使う試験用の中身。ナビゲーションには出さない。
    path: '_test/layout',
    title: $localize`レイアウトの試験 - vehicle-sil`,
    loadComponent: () => import('./testing/layout-fixture').then((m) => m.LayoutFixture),
  },
  {
    path: '**',
    title: $localize`見つからない - vehicle-sil`,
    loadComponent: () => import('./not-found/not-found').then((m) => m.NotFound),
  },
];
