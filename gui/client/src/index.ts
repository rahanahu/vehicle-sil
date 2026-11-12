// vehicle-sil の GUI サーバーのクライアント（`@vehicle-sil/gui-client`）。公開するものは、ここに挙げたものだけ。
// ここにないもの（各ファイルの中の関数や型、`internal.ts`）は、予告なく変わる。枠組み（Angular、RxJS など）に依らない。
// 足すのは簡単、消すのは難しい: 使う人が出てから足す。

// クライアント: 接続、セッションの写し、実行と時刻の操作、サーバーの情報
export { DEFAULT_LIMITS, createClient } from './client';
export type { ClientOptions, GuiClient } from './client';
export type { ClientConnection, CloseInfo, ConnectionState } from './connection';
export type { ClientSession, Notice } from './session';
export type { ClientRun, StopAction } from './run';
export type {
  ClientTime,
  Lateness,
  NotSentReason,
  SendResult,
  TimeFailure,
  TimeState,
} from './time';
export {
  ANSWER_TIMEOUT_MS,
  FEATURE_SPEED,
  FEATURE_START_PAUSED,
  FEATURE_TIME_COMMANDS,
} from './time';

// ECU の変数
export {
  HISTORY_MAX,
  HISTORY_TOTAL_MAX,
  OPERATIONS_KEPT,
  WATCH_PERIODS_MS,
  WATCH_PERIOD_MS,
  WRITE_TIMEOUT_MS,
  WriteError,
} from './variables';
export type {
  ClientVariables,
  HistoryPoint,
  OperationPeer,
  OperationView,
  VariableInfo,
  VariablePeer,
  VariableReading,
  VariableRef,
  VariableValue,
  VariableWatch,
  VariablesState,
  VariablesStatus,
  WatchOptions,
  WatchProblem,
  WatchValues,
  WriteOptions,
} from './variables';

// 読める値
export type { Readable } from './values';

// 差し替えの口（WebSocket、HTTP）
export { BATCH_MS, TRY_AGAIN_LATER, webSocketFactory } from './connection';
export type { SocketFactory, SocketHandlers, SocketLike } from './connection';
export { DEFAULT_TIMEOUT_MS, HttpError, errorMessage, fetchTransport } from './http';
export type { FetchTransportOptions, HttpOutcome, HttpRequest, HttpTransport } from './http';

// セッションの写しの形
export { phaseIndex, runOutcome } from './session-state';
export type {
  CommandView,
  LauncherView,
  LogLine,
  LogSource,
  ParticipantView,
  RunOutcome,
  SessionView,
  TimeView,
} from './session-state';

// サーバーの口の形（正本は gui/protocol/）と、口を自分で読む人のための読み取り
export {
  PHASES,
  PROTOCOL_VERSION,
  ProtocolError,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readServerMessage,
  readSessionResponse,
  readTimeResponse,
  readVariablesSummary,
  readWriteResponse,
  severityRank,
} from './protocol';
export type {
  AppInfo,
  AuthMessage,
  CheckNetwork,
  CheckOutcome,
  CheckParticipant,
  CheckRequest,
  CheckResult,
  CheckStatus,
  CommandReport,
  ConfigFile,
  ConfigList,
  ControlLink,
  ControlOffer,
  ControlState,
  Dropped,
  Finding,
  LauncherEvent,
  LogCounts,
  Open,
  ParticipantState,
  Phase,
  Replay,
  RunParticipant,
  RunRequest,
  RunResult,
  RunResultName,
  RunTiming,
  ServerMessage,
  ServerProblem,
  SessionSummary,
  Severity,
  Speed,
  StopRequest,
  StopRequestBody,
  SummaryCommand,
  SummaryLauncher,
  SummaryParticipant,
  TimeAnswer,
  TimeCommand,
  TimeCommandName,
  TimeReport,
  ValuesMessage,
  Variable,
  VariableSummary,
  VariablesSummary,
  OperationReport,
  WatchMessage,
  WriteAnswer,
} from './protocol';
