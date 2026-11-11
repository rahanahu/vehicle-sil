/**
 * サーバーとの間でやりとりするデータの形（正本）。
 *
 * サーバーとの間の型はここにだけ書き、画面はここから import する。
 * いまは代役のサーバー（dev/stub_server.py）に合わせた仮の形。本物のサーバーの契約ができたら、それに合わせて置き換える。
 */

/** GET api/health の応答。 */
export interface Health {
  status: string;
  server: string;
}

/** WebSocket（ws）で届く 1 つの通知。 */
export interface TelemetrySample {
  seq: number;
  values: Record<string, number>;
}
