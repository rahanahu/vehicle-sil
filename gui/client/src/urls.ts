/**
 * サーバーへの URL の組み立て。
 *
 * URL は先頭に / を付けず、<base href> からの相対で書く（`api/health`、`ws`）。
 * 画面を / 以外のパス（例: /gui/）に置いても、そのまま動く。
 */

/** base href を基準にした WebSocket の URL（http は ws、https は wss）。 */
export function webSocketUrl(baseUri: string, path: string): string {
  const url = new URL(path, baseUri);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.href;
}
