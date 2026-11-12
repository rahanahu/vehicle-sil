import { clearTranslations } from '@angular/localize';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  applyTranslations,
  chooseLanguage,
  languageOfTag,
  setUpLanguage,
  translationsFrom,
} from './language';

describe('chooseLanguage', () => {
  it('takes the language the user chose, when it is supported', () => {
    expect(chooseLanguage('ja', ['en-US'])).toBe('ja');
    expect(chooseLanguage('en', ['ja-JP'])).toBe('en');
  });

  it('ignores a stored value that is not a supported language', () => {
    expect(chooseLanguage('de', ['ja-JP'])).toBe('ja');
    expect(chooseLanguage('', ['ja'])).toBe('ja');
  });

  it('takes the first supported language among the browser languages', () => {
    expect(chooseLanguage(null, ['ja-JP', 'en-US'])).toBe('ja');
    expect(chooseLanguage(null, ['en-GB', 'ja'])).toBe('en');
    expect(chooseLanguage(null, ['de-DE', 'ja'])).toBe('ja');
    expect(chooseLanguage(null, ['JA'])).toBe('ja');
  });

  it('falls back to English when no browser language is supported', () => {
    expect(chooseLanguage(null, ['de-DE', 'fr'])).toBe('en');
    expect(chooseLanguage(null, [])).toBe('en');
  });

  it('reads only the primary subtag', () => {
    expect(languageOfTag('ja-Jpan-JP')).toBe('ja');
    expect(languageOfTag('en_US')).toBe('en');
    expect(languageOfTag('jav')).toBeNull();
    expect(languageOfTag('zh-Hant')).toBeNull();
  });
});

describe('translationsFrom', () => {
  it('keeps the targets that are text, and leaves out the rest', () => {
    const file = {
      locale: 'en',
      translations: {
        a: { source: '開始', target: 'Start' },
        b: { source: '停止', target: null },
        c: { source: '閉じる', target: '' },
        d: { source: '結果', target: '   ' },
        e: { source: '値', target: 3 },
        f: '古い形',
      },
    };
    expect(translationsFrom(file)).toEqual({ a: 'Start' });
  });

  it('reads nothing from a file of another shape', () => {
    expect(translationsFrom(null)).toEqual({});
    expect(translationsFrom({ translations: [] })).toEqual({});
    expect(translationsFrom('text')).toEqual({});
  });
});

describe('the translations at run time', () => {
  afterEach(() => {
    clearTranslations();
    vi.restoreAllMocks();
    localStorage.clear();
  });

  it('translates a loaded message and keeps the source text of the others', () => {
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const name = 'Engine';
    expect(applyTranslations({ 'test-start': 'Start', 'test-use': 'Use {$name}' })).toBe(2);
    expect($localize`:@@test-start:開始`).toBe('Start');
    expect($localize`:@@test-use:${name}:name: を使う`).toBe('Use Engine');
    // 訳のない文は元の日本語（id や空ではない）
    expect($localize`:@@test-missing:停止`).toBe('停止');
  });

  it('falls back to the source text when a translation does not fit its placeholders', () => {
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const name = 'Engine';
    applyTranslations({ 'test-wrong': 'Use {$other}' });
    expect($localize`:@@test-wrong:${name}:name: を使う`).toBe('Engine を使う');
  });

  it('stays in Japanese when the translation file cannot be loaded', async () => {
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    localStorage.setItem('vehicle-sil-gui-language', 'en');
    const fetch = vi.fn().mockResolvedValue(new Response('', { status: 404 }));
    const fake = {
      localStorage,
      navigator: { languages: ['en-US'] },
      document,
      fetch,
    } as unknown as Window;
    expect(await setUpLanguage(fake)).toBe('ja');
    expect(fetch).toHaveBeenCalledWith('locale/messages.en.json', expect.anything());
    expect(document.documentElement.lang).toBe('ja');
  });

  it('loads the translation file of the chosen language', async () => {
    const file = { locale: 'en', translations: { 'test-ok': { source: '了解', target: 'OK' } } };
    const fetch = vi.fn().mockResolvedValue(Response.json(file));
    const fake = {
      localStorage,
      navigator: { languages: ['fr-FR'] },
      document,
      fetch,
    } as unknown as Window;
    expect(await setUpLanguage(fake)).toBe('en');
    expect(document.documentElement.lang).toBe('en');
    expect($localize`:@@test-ok:了解`).toBe('OK');
  });

  it('needs no file for Japanese', async () => {
    const fetch = vi.fn();
    const fake = {
      localStorage,
      navigator: { languages: ['ja-JP'] },
      document,
      fetch,
    } as unknown as Window;
    expect(await setUpLanguage(fake)).toBe('ja');
    expect(fetch).not.toHaveBeenCalled();
  });
});
