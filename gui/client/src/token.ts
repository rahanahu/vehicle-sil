/**
 * サーバーが表示した URL（`http://127.0.0.1:<port>/#token=<...>`）からトークンを受け取る。
 *
 * GUI サーバーが配る画面（サンプルの画面も、`--web` で配る自分の画面も）は、開いた URL の `#token=` を読み、すぐに URL
 * から消し（タブの戻る・進むの履歴からも。閲覧履歴には残ることがある）、タブの sessionStorage に持つ（再読み込みでは残り、タブを閉じると消える）。ブラウザの
 * ものは引数で受け取る（このライブラリは Node でも動くので、`window` を直接読まない）。ブラウザでは `takeToken(window)`。
 */

/** sessionStorage の鍵。同じサーバー（同じ origin）の画面は、この鍵を分け合う。 */
export const TOKEN_STORAGE_KEY = 'vehicle-sil-gui-token';

/** トークンを読む場所。ブラウザの `window` がこの形を持つ。 */
export interface TokenPlace {
  readonly location: { readonly hash: string; readonly pathname: string; readonly search: string };
  readonly history: {
    readonly state: unknown;
    replaceState(data: unknown, unused: string, url?: string | null): void;
  };
  /** 読むだけで投げることがある（ブラウザがサイトのデータを禁じているとき）。そのときはメモリだけに持つ。 */
  readonly sessionStorage?: {
    getItem(key: string): string | null;
    setItem(key: string, value: string): void;
  } | null;
}

/**
 * URL の `#token=` を読んで URL から消し、sessionStorage に置いて返す。URL になければ、sessionStorage に置いたもの
 * （同じタブで前に受け取ったもの）を返す。どちらにもなければ null（サーバーが表示した URL から開いていない）。
 * URL を書き換える前に呼ぶ（画面の経路が URL を書き換えると、`#token=` が消える）。
 */
export function takeToken(place: TokenPlace): string | null {
  const storage = storageOf(place);
  const match = /(?:^#|&)token=([^&]*)/.exec(place.location.hash);
  if (!match) {
    return read(storage);
  }
  // 読めない値でも URL からは消す（タブの履歴や、後で付けるブックマークに残さない）
  place.history.replaceState(
    place.history.state,
    '',
    place.location.pathname + place.location.search,
  );
  let token: string | null;
  try {
    token = decodeURIComponent(match[1]) || null;
  } catch {
    token = null;
  }
  if (token === null) {
    return read(storage);
  }
  try {
    storage?.setItem(TOKEN_STORAGE_KEY, token);
  } catch {
    // 置けなくても、メモリにはある
  }
  return token;
}

function storageOf(place: TokenPlace): TokenPlace['sessionStorage'] {
  try {
    return place.sessionStorage ?? null;
  } catch {
    return null;
  }
}

function read(storage: TokenPlace['sessionStorage']): string | null {
  try {
    return storage?.getItem(TOKEN_STORAGE_KEY) ?? null;
  } catch {
    return null;
  }
}
