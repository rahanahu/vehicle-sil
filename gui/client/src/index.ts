// Client for the vehicle-sil GUI server (`@vehicle-sil/gui-client`). Only what this file exports is public.
// Anything else (functions and types inside the other files, `internal.ts`) may change without notice. No dependency
// on a framework (Angular, RxJS and so on). Adding is easy, removing is hard: add an export once someone needs it.

// Client: connection, session copy, run and time control, server information
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
export { ANSWER_TIMEOUT_MS } from './time';

// Front door: `connect()` returns it. Built on the client above (`front.low`); the client keeps its names and shapes.
export { connect, createFront, until, watch } from './front';
export type { ConnectOptions } from './front';
export { ActionError } from './front-types';
export type {
  ConnectEntry,
  ConnectListError,
  ConnectTarget,
  Control,
  EndKind,
  Front,
  FrontClock,
  FrontControls,
  FrontResult,
  FrontRun,
  FrontScenario,
  FrontStatus,
  FrontTime,
  FrontVariable,
  Intent,
  Link,
  OperationName,
  Outside,
  Pacing,
  Reason,
  RunState,
  VariableView,
} from './front-types';

// Token (the #token= of the URL the server printed)
export { TOKEN_STORAGE_KEY, takeToken } from './token';
export type { TokenPlace } from './token';

// ECU variables
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

// Binding one variable to one widget (client.variables.bind): value state, whether it can be written, write status
export { DROPPED_MESSAGE } from './variable-binding';
export type { BindOptions, BoundVariable, VariableBinding } from './variable-binding';
export { hasRange, isWriteError, valueCondition, writeBlock } from './variable-state';
export type {
  ConditionInput,
  ValueCondition,
  WriteBlock,
  WriteBlockInput,
  WriteStatus,
  WriteValue,
} from './variable-state';

// Read-only values
export type { Readable } from './values';

// Substitutable transports (WebSocket, HTTP)
export { BATCH_MS, TRY_AGAIN_LATER, webSocketFactory } from './connection';
export type { SocketFactory, SocketHandlers, SocketLike } from './connection';
export { DEFAULT_TIMEOUT_MS, HttpError, errorMessage, fetchTransport } from './http';
export type { FetchTransportOptions, HttpOutcome, HttpRequest, HttpTransport } from './http';

// Shape of the session copy
export { isHolding, launcherAnswer, phaseIndex, runOutcome } from './session-state';
export type {
  CommandView,
  LauncherAnswer,
  LauncherView,
  LogLine,
  LogSource,
  ParticipantView,
  RunOutcome,
  ScenarioView,
  SessionView,
  TimeView,
} from './session-state';

// Shapes of the server API (gui/protocol/ is the authority), and parsers for those who read the API themselves
export {
  PHASES,
  PROTOCOL_VERSION,
  ProtocolError,
  readAppInfo,
  readCheckResponse,
  readConfigList,
  readConfigText,
  readSavedConfig,
  readConnections,
  readDashboardResponse,
  readRecording,
  readScenarioList,
  readServerMessage,
  readSessionAllowed,
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
  CheckScenario,
  CheckStatus,
  CheckTextRequest,
  Caution,
  CommandReport,
  ConfigFile,
  ConfigList,
  ConfigText,
  SaveConfigRequest,
  SavedConfig,
  Connections,
  ConnectionsRequest,
  ControlLink,
  ControlOffer,
  ControlState,
  DashboardFile,
  DashboardFileError,
  DashboardFileItem,
  DashboardFileSection,
  DashboardPanelModule,
  DashboardResponse,
  DescribeRequest,
  DashboardVariable,
  Dropped,
  Finding,
  FromLauncher,
  LauncherAllowedItem,
  LauncherEvent,
  LauncherFields,
  LogCounts,
  Open,
  OutsideState,
  ParticipantRole,
  ParticipantState,
  Phase,
  Recording,
  Replay,
  RunParticipant,
  RunRequest,
  RunResult,
  RunEnd,
  RunResultName,
  RunTiming,
  RunTraffic,
  ScenarioFile,
  ScenarioList,
  ScenarioResult,
  ScenarioStatus,
  ScenarioStep,
  ScenarioStepStatus,
  ServerAllowed,
  ServerAllowedItem,
  ServerSaveItem,
  ServerMessage,
  ServerProblem,
  SessionSummary,
  Severity,
  Speed,
  StateReport,
  StopRequest,
  StopRequestBody,
  StopSessionRequestBody,
  SummaryCommand,
  SummaryLauncher,
  SummaryParticipant,
  SummaryScenario,
  SummaryState,
  TimeAnswer,
  TimeCommand,
  TimeCommandName,
  TimeReport,
  TrafficNetwork,
  TrafficParticipant,
  TrafficUnplaced,
  ValuesMessage,
  Variable,
  VariableSummary,
  VariablesSummary,
  OperationReport,
  VariableAvailability,
  WatchMessage,
  WriteAnswer,
} from './protocol';

// Reusable view state for custom monitoring screens.
export { createWaveforms } from './waveforms';
export type { Waveforms, WaveformSeries } from './waveforms';
export { createVariableEditor } from './variable-editor';
export type { VariableEditor, VariableEditorState } from './variable-editor';
