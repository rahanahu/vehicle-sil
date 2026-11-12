import { LOCALE_ID } from '@angular/core';
import { bootstrapApplication } from '@angular/platform-browser';
import { App } from './app/app';
import { appConfig } from './app/app.config';
import { LanguageCode } from './app/core/i18n/language';

/** 決めた言語でアプリを起動する（main.ts が翻訳を読み込んだ後に読み込む）。 */
export function start(language: LanguageCode) {
  return bootstrapApplication(App, {
    ...appConfig,
    providers: [...appConfig.providers, { provide: LOCALE_ID, useValue: language }],
  });
}
