import { Routes } from '@angular/router';

export const routes: Routes = [
  {
    path: '',
    pathMatch: 'full',
    title: $localize`実行 - vehicle-sil`,
    loadComponent: () => import('./run/run-page').then((m) => m.RunPage),
  },
  {
    path: 'variables',
    title: $localize`変数 - vehicle-sil`,
    loadComponent: () => import('./variables/variables-page').then((m) => m.VariablesPage),
  },
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
