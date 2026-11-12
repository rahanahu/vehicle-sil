// 互換の約束の外（`@vehicle-sil/gui-client/internal`）。ライブラリの中の部品と、試験が使うもの。予告なく変わり、
// 消えることもある。アプリの本番のコードからは import しない（gui/web の lint が止める）。要るものがあれば、
// 公開の入口（index.ts）に足すかを決める。
export { Connection, backoffMs } from './connection';
export type { ConnectionOptions } from './connection';
export { SessionFeed, MAX_NOTICES, STALE_EVENTS_NOTICE } from './session';
export { RunControl } from './run';
export { TimeControl } from './time';
export { defaultOrigin, isNode } from './client';
export { webSocketUrl } from './urls';
export { Writable, callEach, derived, rethrowLater, shallowEqual } from './values';
export type { ErrorSink } from './values';
export {
  MAX_FINDINGS,
  MAX_LOG_LINES,
  MAX_LOG_LINES_PER_SOURCE,
  MAX_PROBLEMS,
  addDropped,
  addProblem,
  applyEvents,
  applySummary,
  sessionFromSummary,
  timeView,
} from './session-state';
export {
  CONTROL_CHECK_KEY,
  CONTROL_EVENT,
  CONTROL_RESULT_KEY,
  CONTROL_SUMMARY_KEY,
  Unreadable,
  readCheckResult,
  readFinding,
  readLauncherEvent,
  readRunResult,
  readServerProblem,
  readSessionSummary,
} from './protocol';
export type { Reader } from './protocol';
