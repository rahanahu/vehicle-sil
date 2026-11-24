// For tests: builds a SessionView from a summary. Do not import it from production code.
import { LauncherEvent, SessionSummary } from '../protocol';
import { SessionView, applyEvents, applySummary, sessionFromSummary } from '../session-state';

/**
 * Builds a session copy (SessionView) from a server summary (the summary in hello). Summaries passed after it are
 * applied in order as WebSocket `session` notifications, and events as `event` notifications (just as the library
 * applies notifications). Use it in the tests of your own widgets to build the state they display.
 */
export function sessionViewFrom(
  summary: SessionSummary,
  ...later: readonly (SessionSummary | readonly LauncherEvent[])[]
): SessionView {
  let view = sessionFromSummary(summary);
  for (const next of later) {
    view = Array.isArray(next)
      ? applyEvents(view, next as readonly LauncherEvent[])
      : applySummary(view, next as SessionSummary);
  }
  return view;
}
