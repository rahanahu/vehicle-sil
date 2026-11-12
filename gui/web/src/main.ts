/// <reference types="@angular/localize" />

import { setUpLanguage } from './app/core/i18n/language';

// 翻訳を読み込んでから、アプリのモジュールを読み込む（import()）。$localize はモジュールを評価したときに訳を
// 決めるので（run/labels.ts の表など）、アプリのモジュールを先に読み込むと訳が効かない。
setUpLanguage(window)
  .then(async (language) => (await import('./start')).start(language))
  .catch((err) => console.error(err));
