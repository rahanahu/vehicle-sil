import { DOCUMENT, Injectable, inject, signal } from '@angular/core';

/** sessionStorage の鍵。タブを閉じると消え、再読み込みでは残る。 */
const STORAGE_KEY = 'vehicle-sil-gui-token';

/**
 * サーバーが起動のたびに作るトークン。
 *
 * サーバーは `http://127.0.0.1:<port>/#token=<...>` を表示する。開いた URL の `#token=` を読み、すぐに URL から消し
 * （履歴にも残さない）、メモリと sessionStorage に持つ。API は Authorization の Bearer、WebSocket は最初のメッセージで送る。
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
    const location = this.document.location;
    const window = this.document.defaultView;
    const match = /(?:^#|&)token=([^&]+)/.exec(location.hash);
    if (!match) {
      return this.storage()?.getItem(STORAGE_KEY) ?? null;
    }
    const token = decodeURIComponent(match[1]);
    window?.history.replaceState(window.history.state, '', location.pathname + location.search);
    this.storage()?.setItem(STORAGE_KEY, token);
    return token;
  }

  /** sessionStorage。使えない（禁止されている）ときは null で、メモリだけに持つ。 */
  private storage(): Storage | null {
    try {
      return this.document.defaultView?.sessionStorage ?? null;
    } catch {
      return null;
    }
  }
}
