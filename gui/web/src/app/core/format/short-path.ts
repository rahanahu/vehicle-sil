/** 短い表示に残す末尾の要素の数。 */
const TAIL = 2;

/**
 * 長い絶対パスを短く見せる（全体は title とコピーで渡す）。
 * - ホームのディレクトリ（/home/<名前>、/Users/<名前>、C:\Users\<名前>）は `~`。
 * - 要素が多ければ、先頭と末尾の 2 つだけを残し、間を `…` にする。
 */
export function shortPath(path: string): string {
  const separator = path.includes('\\') && !path.includes('/') ? '\\' : '/';
  const home = /^(\/home\/[^/]+|\/Users\/[^/]+|[A-Za-z]:\\Users\\[^\\]+)(?=$|[\\/])/.exec(path);
  const rest = home ? `~${path.slice(home[0].length)}` : path;
  const parts = rest.split(separator);
  // 先頭（'~'、''（/ の前）、'C:'）と、その次の 1 つは残す
  const head = parts[0] === '' ? 2 : 1;
  if (parts.length <= head + TAIL + 1) return rest;
  return [...parts.slice(0, head), '…', ...parts.slice(-TAIL)].join(separator);
}
