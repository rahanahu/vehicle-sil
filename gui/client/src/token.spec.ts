import { TOKEN_STORAGE_KEY, TokenPlace, takeToken } from './token';

function place(url: string, stored: Record<string, string> = {}) {
  const at = new URL(url);
  const replaced: string[] = [];
  const storage = new Map(Object.entries(stored));
  const value: TokenPlace = {
    location: at,
    history: {
      state: { kept: true },
      replaceState: (_data, _unused, next) => {
        replaced.push(next ?? '');
      },
    },
    sessionStorage: {
      getItem: (key) => storage.get(key) ?? null,
      setItem: (key, item) => void storage.set(key, item),
    },
  };
  return { value, replaced, storage };
}

describe('takeToken', () => {
  it('takes the token from the URL, removes it from the URL and keeps it for a reload', () => {
    const p = place('http://127.0.0.1:8765/some/path?x=1#token=abc-_123');
    expect(takeToken(p.value)).toBe('abc-_123');
    expect(p.replaced).toEqual(['/some/path?x=1']);
    expect(p.storage.get(TOKEN_STORAGE_KEY)).toBe('abc-_123');
  });

  it('reads the kept token when the URL has none', () => {
    const p = place('http://127.0.0.1:8765/', { [TOKEN_STORAGE_KEY]: 'kept' });
    expect(takeToken(p.value)).toBe('kept');
    expect(p.replaced).toEqual([]);
  });

  it('is null when the page was not opened from the server URL', () => {
    expect(takeToken(place('http://127.0.0.1:8765/').value)).toBeNull();
  });

  it('removes a token it cannot read from the URL, and uses the kept one', () => {
    const p = place('http://127.0.0.1:8765/#token=%E0', { [TOKEN_STORAGE_KEY]: 'kept' });
    expect(takeToken(p.value)).toBe('kept');
    expect(p.replaced).toEqual(['/']);
  });

  it('works without a storage, and when the storage throws', () => {
    const p = place('http://127.0.0.1:8765/#token=t');
    expect(takeToken({ ...p.value, sessionStorage: null })).toBe('t');
    const throwing = {
      ...place('http://127.0.0.1:8765/#token=u').value,
      get sessionStorage(): never {
        throw new Error('the site data is blocked');
      },
    };
    expect(takeToken(throwing)).toBe('u');
  });
});
