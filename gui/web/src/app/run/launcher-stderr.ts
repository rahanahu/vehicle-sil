import { ServerProblem } from '../core/api';

/** ランチャーが標準エラーに書いた行の知らせ。 */
export const STDERR_CODE = 'launcher-stderr';

/** サーバーが 1 行ずつの知らせの頭に付ける文（画面では 1 つのまとまりにするので外す）。 */
const PREFIX = 'the launcher wrote: ';

/**
 * ランチャーの標準エラーを 1 つのまとまりにしたもの。サーバーは最初の数行をすぐに、残りは最後の数行だけを
 * 終わりに送り、間で落とした行があれば、その数の知らせ（`detail.skipped`）を間に挟む。
 */
export interface LauncherStderr {
  /** 落とした行より前の行（落とした行がなければ空）。 */
  first: readonly string[];
  /** 間で落とした行の数（なければ 0）。 */
  skipped: number;
  /** 最後の行（落とした行がなければ、すべての行）。失敗の理由はたいていここにある。 */
  last: readonly string[];
}

/** セッションの知らせから、ランチャーの標準エラーを取り出す。なければ null。 */
export function launcherStderr(problems: readonly ServerProblem[]): LauncherStderr | null {
  const lines = problems.filter((p) => p.code === STDERR_CODE);
  if (lines.length === 0) return null;
  const text = (p: ServerProblem) =>
    p.message.startsWith(PREFIX) ? p.message.slice(PREFIX.length) : p.message;
  const gap = lines.findIndex((p) => p.skipped !== undefined);
  if (gap < 0) return { first: [], skipped: 0, last: lines.map(text) };
  return {
    first: lines.slice(0, gap).map(text),
    skipped: lines[gap].skipped ?? 0,
    last: lines.slice(gap + 1).map(text),
  };
}
