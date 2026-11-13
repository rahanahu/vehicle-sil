import { DOCUMENT, Injectable, inject, signal } from '@angular/core';
import { takeToken } from '@vehicle-sil/gui-client';

/**
 * サーバーが起動のたびに作るトークン。
 *
 * サーバーは `http://127.0.0.1:<port>/#token=<...>` を表示する。開いた URL の `#token=` を読み、すぐに URL から消し
 * （履歴にも残さない）、メモリと sessionStorage に持つ（ライブラリの `takeToken`）。API は Authorization の Bearer、
 * WebSocket は最初のメッセージで送る。
 */
@Injectable({ providedIn: 'root' })
export class AuthToken {
  private readonly document = inject(DOCUMENT);
  private readonly current = signal<string | null>(this.take());
  private readonly refused = signal(false);

  /** いまのトークン。なければ null（サーバーが表示した URL から開いていない）。 */
  readonly token = this.current.asReadonly();

  /** サーバーがトークンを断った（サーバーを起動し直した、など）。 */
  readonly rejected = this.refused.asReadonly();

  /** サーバーが断ったことを知らせる（API の 401）。 */
  markRejected(): void {
    this.refused.set(true);
  }

  private take(): string | null {
    const window = this.document.defaultView;
    return window ? takeToken(window) : null;
  }
}
