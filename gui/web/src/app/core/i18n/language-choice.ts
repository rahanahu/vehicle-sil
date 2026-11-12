import { DOCUMENT, DestroyRef, Injectable, InjectionToken, LOCALE_ID, inject } from '@angular/core';
import { LANGUAGES, LanguageCode, languageOfTag, SOURCE_LANGUAGE, storeLanguage } from './language';

/** 読み込み直す（試験で差し替える）。 */
export const PAGE_RELOAD = new InjectionToken<() => void>('PAGE_RELOAD', {
  providedIn: 'root',
  factory: () => {
    const window = inject(DOCUMENT).defaultView;
    return () => window?.location.reload();
  },
});

/** 言語を変えて読み込み直すときに、画面の値を持ち越す sessionStorage の鍵。 */
const CARRY_KEY = 'vehicle-sil-gui-carry';

/**
 * 画面の言語と、その切り替え。
 *
 * 翻訳はアプリの起動の前に読み込む（`$localize` は文を最初に評価したときに訳を決める）ので、言語を変えるとアプリを
 * 読み込み直す。実行はサーバーの中で続き、画面はつなぎ直して受け直す。サーバーにない画面の値（まだ送っていない
 * 選択など）は、画面が `keep` で渡し、読み込み直した後に `take` で受け取る。
 */
@Injectable({ providedIn: 'root' })
export class LanguageChoice {
  private readonly window = inject(DOCUMENT).defaultView;
  private readonly reload = inject(PAGE_RELOAD);
  private readonly locale = inject(LOCALE_ID);
  private readonly keepers = new Map<string, () => unknown>();
  private readonly carried = this.readCarried();

  readonly languages = LANGUAGES;
  /** いま出している言語（LOCALE_ID から）。 */
  readonly current: LanguageCode = languageOfTag(this.locale) ?? SOURCE_LANGUAGE;

  /** 言語を選ぶ。いまと違えば覚えて、画面の値を持ち越して読み込み直す。 */
  choose(code: LanguageCode): void {
    if (code === this.current || !this.window) return;
    if (!storeLanguage(this.window, code)) {
      console.warn('the language choice cannot be remembered (localStorage is not available)');
    }
    const carry: Record<string, unknown> = {};
    for (const [key, snapshot] of this.keepers) carry[key] = snapshot();
    try {
      this.window.sessionStorage.setItem(CARRY_KEY, JSON.stringify(carry));
    } catch {
      // 持ち越せなくても言語は変える（画面の値はサーバーのものに戻る）
    }
    this.reload();
  }

  /** 言語を変えて読み込み直すときに持ち越す値を渡す。`destroyRef` が壊れたら渡すのをやめる。 */
  keep(key: string, snapshot: () => unknown, destroyRef: DestroyRef): void {
    this.keepers.set(key, snapshot);
    destroyRef.onDestroy(() => {
      if (this.keepers.get(key) === snapshot) this.keepers.delete(key);
    });
  }

  /** 前の読み込みから持ち越した値（1 回だけ。なければ undefined）。形は受け取る側が確かめる。 */
  take(key: string): unknown {
    const value = this.carried.get(key);
    this.carried.delete(key);
    return value;
  }

  /** 持ち越した値を読み、すぐに消す（後で利用者が読み込み直したときには使わない）。 */
  private readCarried(): Map<string, unknown> {
    try {
      const storage = this.window?.sessionStorage;
      const text = storage?.getItem(CARRY_KEY);
      storage?.removeItem(CARRY_KEY);
      const value: unknown = text ? JSON.parse(text) : null;
      return typeof value === 'object' && value !== null
        ? new Map(Object.entries(value))
        : new Map();
    } catch {
      return new Map();
    }
  }
}
