// 実行の画面の状態を、サーバーの要約とランチャーの事象から組み立てる純粋な関数。サービス（run-session.ts）が使う。
import {
  CheckResult,
  Finding,
  LauncherEvent,
  PHASES,
  ParticipantState,
  Phase,
  Replay,
  RunResult,
  ServerProblem,
  SessionSummary,
  Speed,
} from '../core/api';

/** 画面に残すログの行の上限。超えたら古い行から落とす。 */
export const MAX_LOG_LINES = 5000;

/** 1 つのログの行。 */
export interface LogLine {
  seq: number;
  /** participant の名前か `vehicle_sil_launch`。 */
  source: string;
  line: string;
}

/** 実行中の participant の様子。 */
export interface ParticipantView {
  name: string;
  /** 最後に届いた状態。まだ何も届いていなければ null。 */
  state: ParticipantState | null;
  required: boolean | null;
  pid: number | null;
  exitCode: number | null;
  signal: string | null;
}

/** 1 つのセッション（ランチャーの 1 回の run）の、画面から見た状態。 */
export interface SessionView {
  /** サーバーが付けたセッションの番号。 */
  id: number;
  /** 最後に受け取った事象の seq（まだなければ -1）。再接続のとき、この次から再送を頼む。 */
  lastSeq: number;
  /** 構成ファイル（root からの相対）。 */
  file: string;
  /** 外した participant。 */
  disable: readonly string[];
  speed: Speed;
  phase: Phase | null;
  check: CheckResult | null;
  participants: readonly ParticipantView[];
  logs: readonly LogLine[];
  /** 落としたログの行の数（画面の上限と、サーバーが覚えておく数の上限の両方）。 */
  droppedLogLines: number;
  /** 実行中に決まった所見（照合の所見は check の中）。 */
  findings: readonly Finding[];
  /** 最後に届いた事象の t_ms（ランチャーの開始からの実時間）。 */
  elapsedMs: number;
  /** 仮想時刻（time の事象が来たときだけ）。 */
  simMs: number | null;
  /** 停止を頼んだ回数（1 で「止めています」、2 で「待たずに止める」を頼んだ後）。 */
  stopRequests: number;
  /** 最後に断られた命令の理由。 */
  rejectedCommand: string | null;
  result: RunResult | null;
  /** サーバーが「ランチャーはもう動いていない」と言った（result を出さずに終わったときも）。 */
  closed: boolean;
  /** サーバーが気づいたこと（英語のまま出す）。 */
  problems: readonly ServerProblem[];
}

/** サーバーの要約からセッションを作る（hello、新しいセッション）。ログは続く再送で届く。 */
export function sessionFromSummary(summary: SessionSummary, replay: Replay | null): SessionView {
  const omitted = replay ? Object.values(replay.omittedLog).reduce((sum, n) => sum + n, 0) : 0;
  const blank: SessionView = {
    id: summary.id,
    lastSeq: -1,
    file: summary.file,
    disable: summary.disable,
    speed: summary.speed,
    phase: null,
    check: null,
    participants: [],
    logs: [],
    droppedLogLines: omitted,
    findings: [],
    elapsedMs: 0,
    simMs: null,
    stopRequests: 0,
    rejectedCommand: null,
    result: null,
    closed: false,
    problems: [],
  };
  return applySummary(blank, summary, true);
}

/**
 * 要約を当てる。終わったか、停止の回数、サーバーの知らせは、いつも要約に従う。
 * 段階、participant、所見、結果は、要約が手元の事象より新しいとき（事象を取りこぼしたとき）か force のときだけ取る。
 */
export function applySummary(
  session: SessionView,
  summary: SessionSummary,
  force = false,
): SessionView {
  const next: SessionView = {
    ...session,
    closed: summary.state === 'ended',
    stopRequests: Math.max(session.stopRequests, summary.stop_requests),
    problems: summary.problems,
  };
  if (!force && (summary.last_seq ?? -1) <= session.lastSeq) {
    return next;
  }
  return {
    ...next,
    phase: summary.phase ?? next.phase,
    simMs: summary.sim_ms ?? next.simMs,
    findings: summary.findings,
    result: summary.result ?? next.result,
    check: next.check ?? summary.result?.check ?? null,
    participants: summary.participants.map((p) => ({
      name: p.name,
      state: p.state,
      required: p.required,
      pid: p.pid,
      exitCode: p.exit_code,
      signal: p.signal,
    })),
  };
}

/** サーバーの知らせを足す（後の要約にも同じものが入り、そちらで置き換わる）。 */
export function addProblem(session: SessionView, problem: ServerProblem): SessionView {
  return { ...session, problems: [...session.problems, problem] };
}

/** 段階の位置（知らない段階は -1）。 */
export function phaseIndex(phase: Phase | null): number {
  return phase === null ? -1 : (PHASES as readonly string[]).indexOf(phase);
}

/** 終わったか（サーバーが終わったと言った、ended の段階、result が届いた）。 */
export function isEnded(session: SessionView): boolean {
  return session.closed || session.result !== null || session.phase === 'ended';
}

/**
 * 事象をまとめて当てる。同じ seq か、それより前の事象（再送の重なり）は無視する。
 * ログの配列は 1 回のまとまりで 1 回だけ作り直す（届くたびに写さない）。
 */
export function applyEvents(session: SessionView, events: readonly LauncherEvent[]): SessionView {
  let next = session;
  const logs: LogLine[] = [];
  for (const event of events) {
    if (event.seq > next.lastSeq) {
      next = applyOne(next, event, logs);
    }
  }
  if (logs.length > 0) {
    const all = [...next.logs, ...logs];
    const over = Math.max(0, all.length - MAX_LOG_LINES);
    next.logs = over > 0 ? all.slice(over) : all;
    next.droppedLogLines += over;
  }
  return next;
}

function applyOne(session: SessionView, event: LauncherEvent, logs: LogLine[]): SessionView {
  const next: SessionView = { ...session, lastSeq: event.seq, elapsedMs: event.t_ms };
  switch (event.event) {
    case 'phase': {
      // 段階は戻らない（要約で先の段階を知っているとき、再送の古い事象で戻さない）
      const known = phaseIndex(next.phase);
      const index = phaseIndex(event.phase);
      return index < 0 || index >= known ? { ...next, phase: event.phase } : next;
    }
    case 'check':
      return {
        ...next,
        check: event.check,
        participants: mergeCheck(next.participants, event.check),
      };
    case 'participant':
      return { ...next, participants: updateParticipant(next.participants, event) };
    case 'log':
      logs.push({ seq: event.seq, source: event.source, line: event.line });
      return next;
    case 'finding':
      return { ...next, findings: addFinding(next.findings, event.finding) };
    case 'time':
      return { ...next, simMs: event.sim_ms };
    case 'command':
      return event.accepted ? next : { ...next, rejectedCommand: event.message ?? event.command };
    case 'result':
      return {
        ...next,
        result: event.result,
        phase: 'ended',
        check: next.check ?? event.result.check,
        participants: mergeResult(next.participants, event.result),
      };
    default:
      return next;
  }
}

/** 同じ所見（要約から入れたもの）を 2 回並べない。 */
function addFinding(findings: readonly Finding[], finding: Finding): readonly Finding[] {
  const same = (f: Finding) =>
    f.code === finding.code &&
    f.message === finding.message &&
    f.participant === finding.participant &&
    f.line === finding.line;
  return findings.some(same) ? findings : [...findings, finding];
}

/** 照合の結果から participant の並び（構成ファイルの順）と、必須かを決める。届いている状態は残す。 */
function mergeCheck(
  current: readonly ParticipantView[],
  check: CheckResult,
): readonly ParticipantView[] {
  const byName = new Map(current.map((p) => [p.name, p]));
  const ordered: ParticipantView[] = [];
  for (const p of check.participants) {
    if (p.name === null) continue;
    const known = byName.get(p.name);
    byName.delete(p.name);
    ordered.push({
      ...(known ?? blankParticipant(p.name)),
      required: p.required,
      state: known?.state ?? (p.enabled === false ? 'disabled' : null),
    });
  }
  return [...ordered, ...byName.values()];
}

function updateParticipant(
  current: readonly ParticipantView[],
  event: Extract<LauncherEvent, { event: 'participant' }>,
): readonly ParticipantView[] {
  const known = current.find((p) => p.name === event.name);
  const updated: ParticipantView = {
    ...(known ?? blankParticipant(event.name)),
    state: event.state,
  };
  if (event.pid !== null) updated.pid = event.pid;
  if (event.state === 'ended' || event.state === 'killed') {
    updated.exitCode = event.exit_code;
    updated.signal = event.signal;
  }
  return known ? current.map((p) => (p === known ? updated : p)) : [...current, updated];
}

/** 最後の結果の、participant の終わり方を映す（事象を取りこぼしていても、結果だけで完結する）。 */
function mergeResult(
  current: readonly ParticipantView[],
  result: RunResult,
): readonly ParticipantView[] {
  const merged = [...current];
  for (const p of result.participants) {
    const i = merged.findIndex((known) => known.name === p.name);
    const base = i >= 0 ? merged[i] : blankParticipant(p.name);
    const state: ParticipantState = !p.enabled
      ? 'disabled'
      : p.ended === 'killed'
        ? 'killed'
        : p.ended === 'not_started'
          ? (base.state ?? 'not_started')
          : 'ended';
    const view: ParticipantView = {
      ...base,
      required: p.required,
      state,
      exitCode: p.exit_code,
      signal: p.signal,
    };
    if (i >= 0) merged[i] = view;
    else merged.push(view);
  }
  return merged;
}

function blankParticipant(name: string): ParticipantView {
  return { name, state: null, required: null, pid: null, exitCode: null, signal: null };
}
