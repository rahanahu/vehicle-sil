// Outside the compatibility promise (`@vehicle-sil/gui-client/internal`): the library's internal building blocks and
// what the tests use. They may change or disappear without notice. Do not import them from application code (the
// gui/web lint rejects it). If you need something here, decide whether to add it to the public entry point (index.ts).
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
