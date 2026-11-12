// 画面の言語を決め、その翻訳を読み込む。アプリを起動する前に main.ts が使う。
// このファイルは $localize を評価するモジュール（画面）を読み込まない（翻訳を読み込む前に訳が決まってしまう）。
import { loadTranslations } from '@angular/localize';

/** 画面の言語。名前はその言語自身で書く（読めない言語の画面でも、自分の言語を見つけられる）。 */
export const LANGUAGES = [
  { code: 'ja', name: '日本語' },
  { code: 'en', name: 'English' },
] as const;

export type LanguageCode = (typeof LANGUAGES)[number]['code'];

/** 元の言語（翻訳ファイルが要らない）。 */
export const SOURCE_LANGUAGE: LanguageCode = 'ja';
/** ブラウザの言語に、対応する言語が 1 つもないときの言語。 */
export const DEFAULT_LANGUAGE: LanguageCode = 'en';

/** 利用者が選んだ言語を持つ localStorage の鍵。 */
export const LANGUAGE_STORAGE_KEY = 'vehicle-sil-gui-language';

/** 翻訳ファイルの場所（`<base href>` からの相対）。 */
export const translationUrl = (code: LanguageCode): string => `locale/messages.${code}.json`;

/** 翻訳ファイルを待つ時間。過ぎたら元の言語で出す。 */
export const TRANSLATION_TIMEOUT_MS = 5000;

export function isLanguage(value: unknown): value is LanguageCode {
  return LANGUAGES.some((language) => language.code === value);
}

/** 言語のタグ（`ja-JP`、`en`）を、対応する言語にする。主な部分（`-` の前）だけを見る。対応しなければ null。 */
export function languageOfTag(tag: string): LanguageCode | null {
  const primary = tag.trim().toLowerCase().split(/[-_]/)[0];
  return isLanguage(primary) ? primary : null;
}

/**
 * 画面の言語を決める。
 *
 * 1. 利用者が選んだ言語（`stored`）が対応する言語なら、それ。
 * 2. ブラウザの言語（`preferred`、好みの順）を前から見て、最初に対応する言語。
 * 3. どれも対応しなければ、英語（`DEFAULT_LANGUAGE`）。
 */
export function chooseLanguage(stored: string | null, preferred: readonly string[]): LanguageCode {
  if (isLanguage(stored)) return stored;
  for (const tag of preferred) {
    const language = languageOfTag(tag);
    if (language) return language;
  }
  return DEFAULT_LANGUAGE;
}

/**
 * 翻訳ファイルの中身を、`loadTranslations` に渡す形（id から訳）にする。
 *
 * 翻訳ファイルは `{ "locale": ..., "translations": { "<id>": { "source": "<元の文>", "target": "<訳>" | null } } }`。
 * 訳が文字列でないもの（null は「まだ訳していない」）と空の訳は渡さない。渡さなかった文は元の言語の文で出る。
 */
export function translationsFrom(file: unknown): Record<string, string> {
  const result: Record<string, string> = {};
  if (typeof file !== 'object' || file === null) return result;
  const entries = (file as { translations?: unknown }).translations;
  if (typeof entries !== 'object' || entries === null) return result;
  for (const [id, entry] of Object.entries(entries)) {
    const target =
      typeof entry === 'object' && entry !== null ? (entry as { target?: unknown }).target : null;
    if (typeof target === 'string' && target.trim() !== '') result[id] = target;
  }
  return result;
}

/**
 * 訳を `$localize` に読み込む。1 つずつ読み込み、読めない訳（壊れた形）は飛ばす（その文は元の言語で出る）。
 * 読み込んだ数を返す。
 */
export function applyTranslations(translations: Record<string, string>): number {
  let loaded = 0;
  for (const [id, target] of Object.entries(translations)) {
    try {
      loadTranslations({ [id]: target });
      loaded++;
    } catch (error) {
      console.warn(`translation ${id} is not usable, the source text is shown instead`, error);
    }
  }
  return loaded;
}

function storedLanguage(window: Window): string | null {
  try {
    return window.localStorage.getItem(LANGUAGE_STORAGE_KEY);
  } catch {
    return null; // localStorage を使えない（禁止されている）
  }
}

/** 選んだ言語を覚える。覚えられなければ false。 */
export function storeLanguage(window: Window, code: LanguageCode): boolean {
  try {
    window.localStorage.setItem(LANGUAGE_STORAGE_KEY, code);
    return true;
  } catch {
    return false;
  }
}

async function fetchTranslations(window: Window, code: LanguageCode): Promise<unknown> {
  const response = await window.fetch(translationUrl(code), {
    signal: AbortSignal.timeout(TRANSLATION_TIMEOUT_MS),
    credentials: 'same-origin',
  });
  if (!response.ok) throw new Error(`${translationUrl(code)}: HTTP ${response.status}`);
  return response.json();
}

/**
 * 画面の言語を決め、元の言語でなければ翻訳を読み込み、`<html lang>` を合わせる。実際に出す言語を返す。
 * 翻訳ファイルを読めなければ、元の言語で出す（画面は動く）。アプリの起動より前に 1 回だけ呼ぶ。
 */
export async function setUpLanguage(window: Window): Promise<LanguageCode> {
  let language = chooseLanguage(storedLanguage(window), window.navigator.languages ?? []);
  if (language !== SOURCE_LANGUAGE) {
    try {
      applyTranslations(translationsFrom(await fetchTranslations(window, language)));
    } catch (error) {
      console.warn(
        `the ${language} translation could not be loaded, the screen stays in Japanese`,
        error,
      );
      language = SOURCE_LANGUAGE;
    }
  }
  window.document.documentElement.lang = language;
  return language;
}
