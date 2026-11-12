// セッション（ランチャーの 1 回の run）の状態を、サーバーの要約とランチャーの事象から組み立てる純粋な関数。
// アプリ全体の接続（server-connection.ts）が使い、画面はできた状態を signal で読む。
import {
  CheckResult,
  Dropped,
  Finding,
  LauncherEvent,
  PHASES,
  ParticipantState,
  Phase,
  RunResult,
  ServerProblem,
  SessionSummary,
  Speed,
  StopRequest,
} from '../api';

/** 出どころ（participant かランチャー）ごとに覚えておくログの行の数。サーバーが覚える数と同じ。 */
export const MAX_LOG_LINES_PER_SOURCE = 2000;
/** 全体で覚えておくログの行の数。超えたら、いちばん多く持っている出どころの古い行から落とす。 */
export const MAX_LOG_LINES = 20_000;
/** 覚えておくサーバーの知らせの数。超えた分は数だけ数える。 */
export const MAX_PROBLEMS = 200;
/** 覚えておく実行の所見の数。 */
export const MAX_FINDINGS = 500;

/** 1 つのログの行。 */
export interface LogLine {
  /** 事象の seq。落ちた行の印は、その位置の前の行の seq + 0.5。 */
  seq: number;
  /** participant の名前か `vehicle_sil_launch`。 */
  source: string;
  line: string;
  /** 落ちた行の印（「ここで N 行が落ちた」）のときだけ。launcher: ランチャーが書けなかった行（log_dropped）。 */
  gap?: { count: number; atLeast: boolean; launcher?: boolean };
}

/** 出どころごとのログの数。 */
export interface LogSource {
  /** いま持っている行の数。 */
  lines: number;
  /** 落とした古い行の数（サーバーが覚えていなかった分と、画面が上限で落とした分）。 */
  dropped: number;
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
  /**
   * ランチャーが失敗と決めた理由（failed の事象の reason。プロセスが終わる前にも届く）。後で ended が来ても残る。
   * 失敗と決めていなければ null。
   */
  failure: string | null;
  /** 起動できなかった理由（not_started）か、失敗の所見の文（failed）。英語のまま。 */
  message: string | null;
  /** ランチャーが止まるよう求めた（stop_requested、結果の ended が stopped）。止めた後の終わり方は失敗の色にしない。 */
  stopRequested: boolean;
  /** 要約に入りきらず、サーバーが省いた欄の名前（その後の事象が届けば空）。 */
  omittedFields: readonly string[];
}

/** ランチャーのプロセスの様子（サーバーが見ている）。 */
export interface LauncherView {
  /** 動いているか。結果が届いた後も、終わるまでは true。止められるかはこれで決める。 */
  running: boolean;
  exitCode: number | null;
  /** 終わらせたシグナルの番号（POSIX だけ）。 */
  signal: number | null;
  /** 動いているのに、何も書いてこない秒数（サーバーが心拍で見ている）。書いていれば null。 */
  silentSeconds: number | null;
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
  /** 最後に届いた、知っている段階の位置（知らない段階が来ても、段階を戻さないため）。 */
  phaseRank: number;
  check: CheckResult | null;
  participants: readonly ParticipantView[];
  /** ログの行（seq の順）。 */
  logs: readonly LogLine[];
  /** 出どころごとの、持っている行と落とした行の数。 */
  logSources: Readonly<Record<string, LogSource>>;
  /** 実行中に決まった所見（照合の所見は check の中）。 */
  findings: readonly Finding[];
  /** 最後に届いた事象の t_ms（ランチャーの開始からの実時間）。 */
  elapsedMs: number;
  /** 仮想時刻（time の事象が来たときだけ）。 */
  simMs: number | null;
  /** 最後の time の、要約に入りきらずサーバーが省いた欄の名前（その後の事象が届けば空）。 */
  timeOmittedFields: readonly string[];
  /** 停止を頼んだ回数（1 で「止めています」、2 で「待たずに止める」を頼んだ後）。 */
  stopRequests: number;
  /** 最後に断られた命令の理由。 */
  rejectedCommand: string | null;
  result: RunResult | null;
  launcher: LauncherView;
  /** サーバーが気づいたこと（英語のまま出す）。 */
  problems: readonly ServerProblem[];
  /** 上限を超えて覚えていないサーバーの知らせの数。 */
  problemsOmitted: number;
}

/** 実行がいまどうなっているか（状態の帯と結果の表示が使う）。 */
export type RunOutcome =
  /** 動いていて、止めるよう頼んでいない。 */
  | 'running'
  /** 止めるよう頼んだ（participant の終わりを待っている）。 */
  | 'stopping'
  /** 待たずに止めるよう頼んだ。 */
  | 'forcing'
  /** 結果は出たが、ランチャーはまだ動いている（終わるのを待っている）。 */
  | 'finishing'
  /** ランチャーが結果を出して終わった。 */
  | 'ended'
  /** ランチャーが結果を出さずに終わった。 */
  | 'crashed';

export function runOutcome(session: SessionView): RunOutcome {
  if (session.launcher.running) {
    if (session.result) return 'finishing';
    if (session.stopRequests >= 2) return 'forcing';
    return session.stopRequests >= 1 ? 'stopping' : 'running';
  }
  return session.result ? 'ended' : 'crashed';
}

/** サーバーの要約からセッションを作る（hello、新しいセッション）。ログは続く再送で届く。 */
export function sessionFromSummary(summary: SessionSummary): SessionView {
  const blank: SessionView = {
    id: summary.id,
    lastSeq: -1,
    file: summary.file,
    disable: summary.disable,
    speed: summary.speed,
    phase: null,
    phaseRank: -1,
    check: null,
    participants: [],
    logs: [],
    logSources: {},
    findings: [],
    elapsedMs: 0,
    simMs: null,
    timeOmittedFields: [],
    stopRequests: 0,
    rejectedCommand: null,
    result: null,
    launcher: launcherOf(summary),
    problems: [],
    problemsOmitted: 0,
  };
  return applySummary(blank, summary, true);
}

/**
 * 送られなかったログの行（dropped）を数え、その出どころのログの今の位置に「ここで N 行が落ちた」の印を置く。
 * 再送の前（not-kept）なら、再送される行の前に入る。
 */
export function addDropped(session: SessionView, dropped: Dropped): SessionView {
  const counts = Object.entries(dropped.log).filter(([, count]) => count > 0);
  if (counts.length === 0) return session;
  const gaps: LogLine[] = counts.map(([source, count]) => ({
    seq: session.lastSeq + 0.5,
    source,
    line: '',
    gap: { count, atLeast: dropped.at_least.includes(source) },
  }));
  const next = appendLogs(session, gaps);
  const sources = { ...next.logSources };
  for (const [source, count] of counts) {
    sources[source] = { ...sources[source], dropped: sources[source].dropped + count };
  }
  return { ...next, logSources: sources };
}

const STOP_REQUESTS: Record<string, number> = { none: 0, requested: 1, forced: 2 };

/** 停止の依頼を回数にする（知らない値は、頼んだものとして 1）。 */
function stopRequestsOf(stop: StopRequest): number {
  return STOP_REQUESTS[stop] ?? 1;
}

function launcherOf(summary: SessionSummary): LauncherView {
  return {
    running: summary.launcher.running,
    exitCode: summary.launcher.exit_code,
    signal: summary.launcher.signal,
    silentSeconds: summary.launcher.silent_s,
  };
}

/**
 * 要約を当てる。ランチャーが動いているか、停止の依頼、サーバーの知らせは、いつも要約に従う（要約が正）。
 * 段階、照合、participant、所見、結果、仮想時刻は、force（WebSocket の hello と session: 事象と同じ順で届くので、
 * 届いた時点の正）か、要約が手元の事象より新しいとき（HTTP の応答）に要約から取る。ログは手元のまま。
 */
export function applySummary(
  session: SessionView,
  summary: SessionSummary,
  force = false,
): SessionView {
  const next: SessionView = {
    ...session,
    launcher: launcherOf(summary),
    stopRequests: Math.max(session.stopRequests, stopRequestsOf(summary.stop)),
    problems: summary.problems,
    problemsOmitted: summary.problems_omitted,
    logSources: withSources(session.logSources, Object.keys(summary.log)),
  };
  if (!force && (summary.last_seq ?? -1) <= session.lastSeq) {
    return next;
  }
  const phase = summary.phase ?? next.phase;
  const rank = phaseIndex(phase);
  const check = summary.check ?? next.check ?? summary.result?.check ?? null;
  const result = summary.result ?? next.result;
  const participants: readonly ParticipantView[] = summary.participants.map((p) => ({
    name: p.name,
    state: p.state,
    required: null,
    pid: p.pid,
    exitCode: p.exit_code,
    signal: p.signal,
    failure: p.reason,
    message: p.message,
    stopRequested: p.state === 'stop_requested',
    omittedFields: p.omitted_fields,
  }));
  return {
    ...next,
    phase,
    phaseRank: rank >= 0 ? rank : next.phaseRank,
    elapsedMs: Math.max(next.elapsedMs, summary.last_t_ms ?? 0),
    simMs: summary.sim_ms ?? next.simMs,
    timeOmittedFields:
      summary.sim_ms === null ? next.timeOmittedFields : summary.time_omitted_fields,
    findings: summary.findings,
    result,
    check,
    // 必須か、外したかは照合の結果にある（並びも構成ファイルの順に）。結果があれば、終わり方（止めたか）も結果から
    participants: withResult(check ? mergeCheck(participants, check) : participants, result),
  };
}

/** 出どころの一覧に、まだ行のない出どころを足す（選べるように）。 */
function withSources(
  sources: Readonly<Record<string, LogSource>>,
  names: readonly string[],
): Readonly<Record<string, LogSource>> {
  const missing = names.filter((name) => !(name in sources));
  if (missing.length === 0) return sources;
  const next = { ...sources };
  for (const name of missing) next[name] = { lines: 0, dropped: 0 };
  return next;
}

/** サーバーの知らせを足す（後の要約にも同じものが入り、そちらで置き換わる）。上限を超えたら数だけ数える。 */
export function addProblem(session: SessionView, problem: ServerProblem): SessionView {
  if (session.problems.length >= MAX_PROBLEMS) {
    return { ...session, problemsOmitted: session.problemsOmitted + 1 };
  }
  return { ...session, problems: [...session.problems, problem] };
}

/** 段階の位置（知らない段階は -1）。 */
export function phaseIndex(phase: Phase | null): number {
  return phase === null ? -1 : (PHASES as readonly string[]).indexOf(phase);
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
  if (logs.length === 0) return next;
  next = appendLogs(next, logs);
  // ランチャーが書けなかった行（log_dropped）は、その出どころの落とした行に数える
  const gaps = logs.filter((line) => line.gap?.launcher);
  if (gaps.length === 0) return next;
  const sources = { ...next.logSources };
  for (const gap of gaps) {
    const counts = sources[gap.source] ?? { lines: 0, dropped: 0 };
    sources[gap.source] = { ...counts, dropped: counts.dropped + (gap.gap?.count ?? 0) };
  }
  return { ...next, logSources: sources };
}

/** ログの行を足し、出どころごとの上限と全体の上限を超えた分を、古い行から落とす。 */
function appendLogs(session: SessionView, added: readonly LogLine[]): SessionView {
  const sources: Record<string, LogSource> = {};
  for (const [source, counts] of Object.entries(session.logSources))
    sources[source] = { ...counts };
  for (const line of added) {
    const counts = (sources[line.source] ??= { lines: 0, dropped: 0 });
    counts.lines++;
  }
  // 落とす数を出どころごとに決める
  const drop = new Map<string, number>();
  const take = (source: string, count: number) => {
    sources[source].lines -= count;
    sources[source].dropped += count;
    drop.set(source, (drop.get(source) ?? 0) + count);
  };
  let total = 0;
  for (const [source, counts] of Object.entries(sources)) {
    if (counts.lines > MAX_LOG_LINES_PER_SOURCE)
      take(source, counts.lines - MAX_LOG_LINES_PER_SOURCE);
    total += counts.lines;
  }
  while (total > MAX_LOG_LINES) {
    const [largest, second] = Object.entries(sources).sort(([, a], [, b]) => b.lines - a.lines);
    const count = Math.max(
      1,
      Math.min(total - MAX_LOG_LINES, largest[1].lines - (second?.[1].lines ?? 0)),
    );
    take(largest[0], count);
    total -= count;
  }
  const all = [...session.logs, ...added];
  if (drop.size === 0) return { ...session, logs: all, logSources: sources };
  const kept = all.filter((line) => {
    const left = drop.get(line.source);
    if (left === undefined || left === 0) return true;
    drop.set(line.source, left - 1);
    return false;
  });
  return { ...session, logs: kept, logSources: sources };
}

function applyOne(session: SessionView, event: LauncherEvent, logs: LogLine[]): SessionView {
  const next: SessionView = {
    ...session,
    lastSeq: event.seq,
    elapsedMs: Math.max(session.elapsedMs, event.t_ms),
  };
  switch (event.event) {
    case 'phase': {
      // 段階は戻らない（要約で先の段階を知っているとき、再送の古い事象で戻さない）。
      // 知らない段階はそのまま出すが、後に来る知っている段階の比べ先は、最後に知っていた段階
      const index = phaseIndex(event.phase);
      if (index < 0) return { ...next, phase: event.phase };
      return index >= next.phaseRank ? { ...next, phase: event.phase, phaseRank: index } : next;
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
    case 'log_dropped':
      // 落ちた位置に印を置く（ここで N 行が落ちた）。印の seq はこの事象の seq
      for (const [source, count] of Object.entries(event.sources)) {
        if (count > 0) {
          logs.push({
            seq: event.seq,
            source,
            line: '',
            gap: { count, atLeast: false, launcher: true },
          });
        }
      }
      return next;
    case 'finding':
      // 再送は所見を送り直さない（要約が持つ）ので、届いた所見はそのまま足す（同じ文が 2 回出ることもある）
      return next.findings.length >= MAX_FINDINGS
        ? next
        : { ...next, findings: [...next.findings, event.finding] };
    case 'time':
      return { ...next, simMs: event.sim_ms, timeOmittedFields: [] };
    case 'command':
      return event.accepted ? next : { ...next, rejectedCommand: event.message ?? event.command };
    case 'result':
      return {
        ...next,
        result: event.result,
        phase: 'ended',
        phaseRank: phaseIndex('ended'),
        check: next.check ?? event.result.check,
        participants: mergeResult(next.participants, event.result),
      };
    default:
      return next;
  }
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
    // 事象はそのまま届く（省くのは要約だけ）
    omittedFields: [],
  };
  if (event.pid !== null) updated.pid = event.pid;
  if (event.state === 'failed') updated.failure = event.reason ?? '';
  if (event.state === 'stop_requested') updated.stopRequested = true;
  if (event.message) updated.message = event.message;
  if (event.state === 'ended' || event.state === 'killed') {
    updated.exitCode = event.exit_code;
    updated.signal = event.signal;
  }
  return known ? current.map((p) => (p === known ? updated : p)) : [...current, updated];
}

function withResult(
  participants: readonly ParticipantView[],
  result: RunResult | null,
): readonly ParticipantView[] {
  return result ? mergeResult(participants, result) : participants;
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
      stopRequested: base.stopRequested || p.ended === 'stopped',
    };
    if (i >= 0) merged[i] = view;
    else merged.push(view);
  }
  return merged;
}

function blankParticipant(name: string): ParticipantView {
  return {
    name,
    state: null,
    required: null,
    pid: null,
    exitCode: null,
    signal: null,
    failure: null,
    message: null,
    stopRequested: false,
    omittedFields: [],
  };
}
