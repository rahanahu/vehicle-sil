/**
 * Takes the token from the URL the server printed (`http://127.0.0.1:<port>/#token=<...>`).
 *
 * Every screen the GUI server serves (the sample screens, and your own screen served with `--web`) reads `#token=`
 * from the URL it was opened with, removes it from the URL right away (also from the tab's back/forward history; the
 * browsing history may still keep it) and keeps it in the tab's sessionStorage (it survives a reload and is gone when
 * the tab closes). Browser objects come in as arguments (this library runs on Node too, so it does not read `window`
 * directly). In the browser, call `takeToken(window)`.
 */

/** sessionStorage key. Screens of the same server (same origin) share this key. */
export const TOKEN_STORAGE_KEY = 'vehicle-sil-gui-token';

/** Where to read the token from. The browser's `window` has this shape. */
export interface TokenPlace {
  readonly location: { readonly hash: string; readonly pathname: string; readonly search: string };
  readonly history: {
    readonly state: unknown;
    replaceState(data: unknown, unused: string, url?: string | null): void;
  };
  /** Even reading it can throw (when the browser blocks site data); the token is then kept in memory only. */
  readonly sessionStorage?: {
    getItem(key: string): string | null;
    setItem(key: string, value: string): void;
  } | null;
}

/**
 * Reads `#token=` from the URL, removes it from the URL, stores it in sessionStorage and returns it. If the URL has
 * none, returns the one in sessionStorage (received earlier in the same tab). If neither has one, returns null (the
 * page was not opened from the URL the server printed).
 * Call it before anything rewrites the URL (when the screen's router rewrites the URL, `#token=` is lost).
 */
export function takeToken(place: TokenPlace): string | null {
  const storage = storageOf(place);
  const match = /(?:^#|&)token=([^&]*)/.exec(place.location.hash);
  if (!match) {
    return read(storage);
  }
  // Remove it from the URL even if the value is unreadable (keep it out of the tab history and of later bookmarks)
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
    // Storing failed, but the token is still in memory
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
