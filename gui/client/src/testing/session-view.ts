// 試験のための、要約から SessionView を作る関数。本番のコードからは import しない。
import { LauncherEvent, SessionSummary } from '../protocol';
import { SessionView, applyEvents, applySummary, sessionFromSummary } from '../session-state';

/**
 * サーバーの要約（hello の要約）から、セッションの写し（SessionView）を作る。続けて渡した要約は WebSocket の
 * `session` の通知として、事象は `event` の通知として、順に当てる（ライブラリが通知を当てるのと同じ）。自分の部品の
 * 試験で、表示する状態を作るときに使う。
 */
export function sessionViewFrom(
  summary: SessionSummary,
  ...later: readonly (SessionSummary | readonly LauncherEvent[])[]
): SessionView {
  let view = sessionFromSummary(summary);
  for (const next of later) {
    view = Array.isArray(next)
      ? applyEvents(view, next as readonly LauncherEvent[])
      : applySummary(view, next as SessionSummary, true);
  }
  return view;
}
