/**
 * Builds the URLs to the server.
 *
 * URLs have no leading / and are relative to <base href> (`api/app`, `ws`), so a screen served under a path other
 * than / (for example /gui/) works unchanged.
 */

/** WebSocket URL relative to the base href (ws for http, wss for https). */
export function webSocketUrl(baseUri: string, path: string): string {
  const url = new URL(path, baseUri);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.href;
}
