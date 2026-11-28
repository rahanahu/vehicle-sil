import {
  Component,
  DOCUMENT,
  DestroyRef,
  LOCALE_ID,
  computed,
  effect,
  inject,
  linkedSignal,
  resource,
  signal,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCardModule } from '@angular/material/card';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatRadioModule } from '@angular/material/radio';
import { MatSelectModule } from '@angular/material/select';
import {
  AfterScenario,
  CheckResult,
  Intent,
  ScenarioParamValue,
  SessionView,
  Speed,
  errorMessage,
  isLauncherFinishing,
  isRunActive,
} from '@vehicle-sil/gui-client';
import { pageIntent } from '@vehicle-sil/gui-client/elements';
import { AuthToken } from '../core/auth/auth-token';
import { shortPath } from '../core/format/short-path';
import { LanguageChoice } from '../core/i18n/language-choice';
import { ServerInfo } from '../core/session/server-info';
import { SimulationTime } from '../core/session/simulation-time';
import { CheckPanel } from './check-panel';
import { FindingsList } from '../widgets/findings-list';
import { cutShort, failureLine, resultTone } from './labels';
import { STDERR_CODE, launcherStderr } from './launcher-stderr';
import { LogView } from './log-view';
import { OutsideChoice, OutsideRows, connectChoice, connectText } from './outside-connect';
import { ParticipantStatus } from './participant-status';
import { RecordSignals } from './record-signals';
import { RecordingPanel } from './recording-panel';
import { ResultPanel } from './result-panel';
import { RunSession } from './run-session';
import { RunStatus } from './run-status';
import {
  CaseOption,
  caseOptionsFor,
  checkSelectionGuidance,
  isParamRecord,
} from './scenario-param-model';
import { ScenarioParamsPanel } from './scenario-params';
import { ScenarioPanel, timelinePageUrl } from './scenario-panel';
import { SpeedPicker } from './speed-picker';
import { TimeButtons } from './time-buttons';
import { TimePanel } from './time-panel';
import { TimelineView } from './timeline-view';

/** 「使う」を切り替えてから照合を頼むまでの間（ミリ秒）。続けて切り替えたら、最後の組み合わせで 1 回だけ照合する。 */
export const CHECK_DELAY_MS = 300;
/** 失敗後の追加記録（仮想時間 ms）の上限。 */
const RECORD_AFTER_FAIL_MS_MAX = 600_000;

/** 画面に出すサーバーの知らせの種類（code）の数。 */
const MAX_PROBLEM_GROUPS = 50;
/** 画面がほかの場所で日本語で説明している知らせ（no-result は異常終了のカード、launcher-silent は状態の帯）。 */
const EXPLAINED_PROBLEMS = ['no-result', 'launcher-silent'];

/** ランチャー自身のログの出どころ。 */
const LAUNCHER_SOURCE = 'vehicle_sil_launch';
/**
 * ランチャーのログの行で、自分で起動した participant が終わった後もシミュレーションに残っていると知らせるもの
 * （`<name>` の後に続く部分。ランチャーの英語のまま）。
 */
const MANUAL_STAYS = ' (started by you, manual) has not left the simulation';

/**
 * 言語を変えて読み込み直すときに持ち越す、この画面の選択（サーバーにまだ送っていないもの）。外す participant と
 * 速度は、持ち越したときと同じセッション（なければ、セッションがないまま）の間だけ戻す（次の実行の値は上書きしない）。
 */
interface RunChoices {
  session: string | null;
  chosen: string | null;
  /** 外す participant を選んだときのファイル。 */
  file: string | null;
  disable: string[];
  /** 自分で起動する participant（古い持ち越しにはない）。 */
  manual?: string[];
  /** 追加で記録する信号（古い持ち越しにはない）。 */
  record?: string[];
  /** 失敗後の追加記録（仮想時間 ms。古い持ち越しにはない）。 */
  recordAfterFailMs?: string;
  /** 仮想時刻付き ECU イベントを記録するか（古い持ち越しにはない）。 */
  recordEvents?: boolean;
  /** 仮想時刻付き CAN フレームを記録するか（古い持ち越しにはない）。 */
  recordCan?: boolean;
  speed: Speed;
  stopAt?: string;
  stopAtEdited?: boolean;
  stopSession?: string | null;
  /** 選んだバリアント（古い持ち越しにはない）。 */
  variant?: string | null;
  /** 選んだシナリオ（古い持ち越しにはない）。 */
  scenario?: string | null;
  /** 選んだケース（古い持ち越しにはない）。 */
  case?: string | null;
  /** パラメータ上書き（古い持ち越しにはない）。 */
  params?: Record<string, ScenarioParamValue>;
  /** シナリオ終了後の動作（古い持ち越しにはない）。 */
  afterScenario?: AfterScenario;
  /** 止めた状態で始めるか（古い持ち越しにはない）。 */
  startPaused?: boolean;
  logSource: string;
}

const isText = (value: unknown): value is string => typeof value === 'string';

/** 持ち越した値（sessionStorage から来る）を確かめる。形が違えば null（持ち越さない）。 */
export function readRunChoices(value: unknown): RunChoices | null {
  if (typeof value !== 'object' || value === null) return null;
  const v = value as Record<string, unknown>;
  const speedOk = v['speed'] === null || v['speed'] === 'max' || typeof v['speed'] === 'number';
  if (
    !(v['session'] === null || isText(v['session'])) ||
    !(v['chosen'] === null || isText(v['chosen'])) ||
    !(v['file'] === null || isText(v['file'])) ||
    !Array.isArray(v['disable']) ||
    !v['disable'].every(isText) ||
    !(v['manual'] === undefined || (Array.isArray(v['manual']) && v['manual'].every(isText))) ||
    !(v['record'] === undefined || (Array.isArray(v['record']) && v['record'].every(isText))) ||
    !(v['recordAfterFailMs'] === undefined || typeof v['recordAfterFailMs'] === 'string') ||
    !(v['recordEvents'] === undefined || typeof v['recordEvents'] === 'boolean') ||
    !(v['recordCan'] === undefined || typeof v['recordCan'] === 'boolean') ||
    !speedOk ||
    !(v['variant'] === undefined || v['variant'] === null || isText(v['variant'])) ||
    !(v['scenario'] === undefined || v['scenario'] === null || isText(v['scenario'])) ||
    !(v['case'] === undefined || v['case'] === null || isText(v['case'])) ||
    !(v['params'] === undefined || isParamRecord(v['params'])) ||
    !(
      v['afterScenario'] === undefined ||
      v['afterScenario'] === 'stop' ||
      v['afterScenario'] === 'pause'
    ) ||
    !(v['startPaused'] === undefined || typeof v['startPaused'] === 'boolean') ||
    !(v['stopAt'] === undefined || typeof v['stopAt'] === 'string') ||
    !(v['stopAtEdited'] === undefined || typeof v['stopAtEdited'] === 'boolean') ||
    !(v['stopSession'] === undefined || v['stopSession'] === null || isText(v['stopSession'])) ||
    (v['stopAtEdited'] === true && typeof v['stopAt'] !== 'string') ||
    !isText(v['logSource'])
  ) {
    return null;
  }
  return v as unknown as RunChoices;
}

/** 2 つの意図が同じか（JSON で比べる。画面が作る意図は、欄がいつも同じ順）。 */
const sameIntent = (a: Intent, b: Intent) => JSON.stringify(a) === JSON.stringify(b);

/** 中断できる待ち。中断されたら、その理由で失敗する（待ち終えたら、中断の受け口を外す）。 */
function wait(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    signal.throwIfAborted();
    const abort = () => {
      clearTimeout(timer);
      reject(signal.reason);
    };
    const timer = setTimeout(() => {
      signal.removeEventListener('abort', abort);
      resolve();
    }, ms);
    signal.addEventListener('abort', abort, { once: true });
  });
}

/**
 * 実行の画面: 構成ファイルを選び、照合の結果を見て participant を外し、始めて、経過とログを見て、結果を見る。
 *
 * - 実行の状態はサーバーにあり、アプリ全体の接続（ServerConnection）が映す。この画面は RunSession を通して読む。
 * - 上の状態の帯に、操作と結果をいつも出す。詳しいカードはその下（実行した後は、結果、状態、ログを先に）。
 * - 選んだファイル、外す participant、速度は、この画面だけの signal。実行中は、実行しているセッションのものを出す。
 * - 選択から次の実行の意図（`intent`）を 1 か所で作り、照合も開始もそれを正面の口に渡す。画面の欄は `pageIntent`
 *   にも書く（部品 <vsil-connect> の照合が同じ意図になり、同じ照合を分け合う）。
 */
@Component({
  selector: 'app-run-page',
  imports: [
    MatButtonModule,
    MatCardModule,
    MatCheckboxModule,
    MatFormFieldModule,
    MatInputModule,
    MatRadioModule,
    MatSelectModule,
    CheckPanel,
    FindingsList,
    LogView,
    OutsideChoice,
    OutsideRows,
    ParticipantStatus,
    RecordSignals,
    RecordingPanel,
    ResultPanel,
    RunStatus,
    ScenarioPanel,
    ScenarioParamsPanel,
    SpeedPicker,
    TimeButtons,
    TimePanel,
    TimelineView,
  ],
  providers: [RunSession],
  templateUrl: './run-page.html',
  styleUrl: './run-page.css',
})
export class RunPage {
  private readonly document = inject(DOCUMENT);
  protected readonly auth = inject(AuthToken);
  protected readonly run = inject(RunSession);
  private readonly info = inject(ServerInfo);
  /** 時刻（仮想時間、一時停止、速さ）と、その操作。 */
  protected readonly time = inject(SimulationTime);
  private readonly language = inject(LanguageChoice);
  private readonly locale = inject(LOCALE_ID);
  /** 言語を変える前の選択（なければ null）。 */
  private readonly carried = readRunChoices(this.language.take('run'));

  /** サーバーとランチャーの情報（アプリ全体で 1 つ。ほかの部品も ServerInfo から同じものを読む）。 */
  protected readonly app = this.info.app;
  protected readonly configs = resource({
    params: () => (this.auth.rejected() ? undefined : true),
    loader: () => this.info.configs(),
  });

  /**
   * WebSocket の握手で断られたとき、その理由（ブラウザからは見えない）を api/app で確かめる。
   * 断られるたびに 1 回呼ぶ。トークンの誤りは interceptor が「断られた」にする。
   */
  protected readonly handshake = resource({
    params: () => {
      const failures = this.run.handshakeFailures();
      return failures > 0 && !this.auth.rejected() ? failures : undefined;
    },
    loader: () => this.info.fetchApp(),
  });
  protected readonly handshakeError = computed(() => {
    if (this.run.connectionState() === 'open') return null;
    const error = this.handshake.error();
    return error ? errorMessage(error) : null;
  });
  protected readonly session = this.run.session;
  /** 帯の下に出す、最後の開始か停止の失敗（正面の口の `status.problem`。時刻の命令の失敗は時刻のカード）。 */
  protected readonly actionError = computed(() => failureLine(this.run.failure()));
  /** ランチャーが動いている（設定を変えられず、照合もしない）。 */
  protected readonly running = this.run.launcherRunning;

  /** 選べる範囲（root）の短い表示。 */
  protected readonly root = computed(() => {
    if (!this.app.hasValue()) return null;
    const full = this.app.value().root;
    return { full, short: shortPath(full) };
  });
  private readonly copiedPath = signal(false);
  protected readonly copied = this.copiedPath.asReadonly();

  /** 最初に選んでおくファイル: いまのセッションのもの、起動のときに渡されたもの、一覧の最初、の順。 */
  private readonly preferredFile = computed(() => {
    const listed = this.configs.hasValue() ? this.configs.value().files.map((c) => c.path) : [];
    const sessionFile = this.session()?.file ?? null;
    if (sessionFile !== null && listed.includes(sessionFile)) return sessionFile;
    const given = this.app.hasValue() ? this.app.value().initial_file : null;
    return given ?? listed[0] ?? null;
  });

  /** 利用者が選んだファイル。選ぶまでは null で、preferredFile に従う。 */
  private readonly chosen = signal<string | null>(this.carried?.chosen ?? null);
  /**
   * 選んでいる構成ファイル（root からの相対）。利用者が選んだら、後から届いたセッションの知らせなどで変えない。
   */
  protected readonly selected = computed(() => this.chosen() ?? this.preferredFile());

  /** 前の実行と違う構成ファイルを選んでいる（出ている結果は前の実行のもの。選ぶ、照合を先に並べる）。 */
  protected readonly otherFile = computed(() => {
    const session = this.session();
    return (
      !!session &&
      !this.running() &&
      this.run.status().run !== 'starting' &&
      this.selected() !== session.file
    );
  });

  /** 外す participant。ファイルを変えたら、そのファイルのセッションで外したもの（なければ空）に戻す。 */
  protected readonly disabled = linkedSignal<
    { file: string | null; names: readonly string[] },
    ReadonlySet<string>
  >({
    source: () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        names:
          carried && carried.file === file
            ? carried.disable
            : session && session.file === file
              ? session.disable
              : [],
      };
    },
    computation: ({ names }) => new Set(names),
    equal: (a, b) => a.size === b.size && [...a].every((name) => b.has(name)),
  });

  /**
   * 画面で「自分で起動する」にした participant（構成ファイルの manual: true は照合の結果にある）。ファイルを変えたら、
   * そのファイルのセッションで指定したもの（なければ空）に戻す。
   */
  protected readonly manual = linkedSignal<
    { file: string | null; names: readonly string[] },
    ReadonlySet<string>
  >({
    source: () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        names:
          carried && carried.file === file
            ? (carried.manual ?? [])
            : session && session.file === file
              ? session.manual
              : [],
      };
    },
    computation: ({ names }) => new Set(names),
    equal: (a, b) => a.size === b.size && [...a].every((name) => b.has(name)),
  });
  /**
   * 送る「自分で起動する」。外したものと重ならないのは、照合の欄（check-panel）が外すときに消すから（重なれば、
   * ランチャーが照合で断る）。
   */
  private readonly manualToSend = computed(() => [...this.manual()].sort());

  protected readonly speedRange = this.info.limits;
  /**
   * 帯に一時停止と再開を出すか: 始めてから、結果が出るかランチャーが終わるまで。止めるよう頼んだ後も、押せないまま
   * 同じ場所に残す（「待たずに止める」が、一時停止のあった場所に出ないように）。停止の後に並ぶので、出ても消えても
   * 開始と停止は動かない。
   */
  protected readonly launcherFinishing = isLauncherFinishing;
  protected readonly runActive = isRunActive;
  protected readonly runtimeNote = computed<string | null>(() => {
    const rt = this.run.runtime();
    if (!rt) return null;
    if (rt.state === 'preparing') {
      return rt.config
        ? $localize`${rt.config} 用のランチャーを準備しています。`
        : $localize`ランチャーを準備しています。`;
    }
    if (rt.state === 'resetting') return $localize`常駐ランチャーを切り替えています。`;
    if (rt.state === 'ready' && rt.mode === 'hot') {
      return rt.config
        ? $localize`${rt.config} をすぐ開始できます（常駐待機中）。`
        : $localize`常駐ランチャーが待機中です。`;
    }
    if (
      rt.mode === 'cold' &&
      rt.reason &&
      rt.reason !== 'launcher-unsupported' &&
      rt.reason !== 'one-shot-runtime'
    ) {
      if (rt.reason === 'fixed-registry')
        return $localize`固定の registry_uri があるため、実行ごとにランチャーを起動します。`;
      if (rt.reason === 'manual-participants')
        return $localize`手動起動の participant があるため、実行ごとにランチャーを起動します。`;
      if (rt.reason === 'trace-enabled')
        return $localize`トレース有効のため、実行ごとにランチャーを起動します。`;
      if (rt.reason === 'resident-failed')
        return $localize`常駐ランチャーの準備に失敗したため、実行ごとの起動へ切り替えました。`;
    }
    return null;
  });

  protected readonly timeButtonsShown = computed(() => {
    const session = this.session();
    if (this.run.starting()) return true;
    return isRunActive(session) && !session!.result;
  });
  /** 「時刻」のカードを出すか: 実行中（結果が出るかランチャーが終わるまで）。 */
  protected readonly timeCardShown = computed(() => {
    const session = this.session();
    return isRunActive(session) && !session!.result;
  });
  /** 速度。前の実行のもの（なければ等速）から始める。 */
  protected readonly speed = linkedSignal<Speed>(() => {
    const session = this.session();
    const carried = this.carriedFor(session?.id ?? null);
    return (carried ? carried.speed : session?.speed) ?? 1;
  });
  /** 速度の倍率の欄が範囲の外。 */
  protected readonly speedInvalid = signal(false);
  /** 選択中のファイルとセッションを切り替えたら、その実行の上限に戻す。 */
  private readonly stopAtSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.file === file ? session.id : null;
      const carried = this.carriedStopFor(id);
      const sessionStop = session?.file === file ? session.check?.stop_at_ms : undefined;
      return {
        file,
        id,
        value:
          carried && carried.file === file && carried.stopAtEdited
            ? (carried.stopAt ?? '')
            : String(sessionStop ?? ''),
      };
    },
    { equal: (a, b) => a.file === b.file && a.id === b.id && a.value === b.value },
  );
  /** 利用者が編集した空欄と、まだセッションの照合結果を待つ空欄を区別する。 */
  private readonly stopAtEditSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.file === file ? session.id : null;
      const carried = this.carriedStopFor(id);
      return { file, id, edited: !!(carried && carried.file === file && carried.stopAtEdited) };
    },
    { equal: (a, b) => a.file === b.file && a.id === b.id && a.edited === b.edited },
  );
  protected readonly stopAtEdited = linkedSignal({
    source: this.stopAtEditSource,
    computation: ({ edited }) => edited,
  });
  /** 空欄なら自動停止せず、値があれば仮想時刻の上限にする。 */
  protected readonly stopAt = linkedSignal<
    { file: string | null; id: string | null; value: string },
    string
  >({
    source: this.stopAtSource,
    computation: (source, previous) =>
      previous &&
      previous.source.file === source.file &&
      previous.source.id === source.id &&
      this.stopAtEdited()
        ? previous.value
        : source.value,
  });
  protected readonly stopAtTouched = linkedSignal({
    source: this.stopAtEditSource,
    computation: () => false,
  });
  protected readonly stopAtInvalid = computed(() => {
    const value = this.stopAt().trim();
    return (
      value !== '' &&
      (!/^\d+$/.test(value) || Number(value) < 1 || Number(value) > 1_000_000_000_000)
    );
  });
  /**
   * 止めた状態で始める（全員がそろったら仮想時間 0 で止まり、再開かステップ送りを待つ）。前の実行からは持ち越さない
   * （別のタブや前の人が止めて始めたことで、気づかずに止まったまま始まらないように）。この画面の中では選んだまま。
   */
  protected readonly startPaused = signal(this.carried?.startPaused ?? false);

  /** 選んだ構成ファイルの隣の scenarios のシナリオ（ランチャーが動いている間は読まない）。 */
  protected readonly scenarios = resource({
    params: () => {
      const file = this.selected();
      if (
        !this.app.hasValue() ||
        this.run.incompatible() ||
        this.auth.rejected() ||
        file === null ||
        this.running()
      )
        return undefined;
      return { file };
    },
    loader: ({ params }) => this.info.scenarios(params.file),
  });
  /**
   * 選んだバリアント。ファイルか実行が替わったら、その実行のもの（同じファイルのとき）か未選択に戻す。
   */
  private readonly variantSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        id: session?.id ?? null,
        variant:
          carried && carried.file === file && carried.variant !== undefined
            ? carried.variant
            : session && session.file === file
              ? (session.intent?.variant ?? session.check?.variant ?? null)
              : null,
      };
    },
    { equal: (a, b) => a.file === b.file && a.id === b.id },
  );
  protected readonly variantChoice = linkedSignal<{ variant: string | null }, string | null>({
    source: () => this.variantSource(),
    computation: ({ variant }) => variant,
  });

  /**
   * 選んだシナリオ。ファイルか実行が替わったら、その実行のもの（同じファイルのとき）か、なしに戻す（実行の知らせの
   * たびには戻さない）。
   */
  private readonly scenarioSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        id: session?.id ?? null,
        scenario:
          carried && carried.file === file && carried.scenario !== undefined
            ? carried.scenario
            : session && session.file === file
              ? (session.intent?.scenario ?? session.scenario?.file ?? null)
              : null,
      };
    },
    { equal: (a, b) => a.file === b.file && a.id === b.id },
  );
  protected readonly scenarioChoice = linkedSignal<{ scenario: string | null }, string | null>({
    source: () => this.scenarioSource(),
    computation: ({ scenario }) => scenario,
  });
  /** 送るシナリオ: 選んだもので、いまの一覧にあるもの（一覧を読むまでは null）。 */
  protected readonly chosenScenario = computed(() => {
    const choice = this.scenarioChoice();
    if (choice === null || !this.scenarios.hasValue()) return null;
    return this.scenarios.value().scenarios.some((s) => s.path === choice) ? choice : null;
  });

  /** 選んだシナリオのケース。ファイル・セッション・シナリオ切替時にセッションの intent から復元する。 */
  private readonly caseSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.id ?? null;
      const scenario = this.scenarioChoice();
      const carried = this.carriedFor(id);
      const sessionScenario =
        session && session.file === file
          ? (session.intent?.scenario ?? session.scenario?.file ?? null)
          : null;
      return {
        file,
        id,
        scenario,
        value:
          scenario !== null &&
          carried &&
          carried.file === file &&
          carried.scenario === scenario &&
          carried.case !== undefined
            ? carried.case
            : scenario !== null && scenario === sessionScenario
              ? (session?.intent?.case ?? session?.scenario?.case ?? null)
              : null,
      };
    },
    {
      equal: (a, b) =>
        a.file === b.file && a.id === b.id && a.scenario === b.scenario && a.value === b.value,
    },
  );
  protected readonly caseChoice = linkedSignal<
    { file: string | null; id: string | null; scenario: string | null; value: string | null },
    string | null
  >({
    source: this.caseSource,
    computation: ({ value }) => value,
  });

  /** パラメータ上書き。ファイル・セッション・シナリオ・ケース切替時に一致するセッションの intent から復元する。 */
  private readonly paramOverridesSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.id ?? null;
      const scenario = this.scenarioChoice();
      const scenarioCase = this.caseChoice();
      const carried = this.carriedFor(id);
      const sessionScenario =
        session && session.file === file
          ? (session.intent?.scenario ?? session.scenario?.file ?? null)
          : null;
      const sessionCase =
        session && session.file === file
          ? (session.intent?.case ?? session.scenario?.case ?? null)
          : null;
      const value: Readonly<Record<string, ScenarioParamValue>> =
        scenario !== null &&
        carried &&
        carried.file === file &&
        carried.scenario === scenario &&
        (carried.case ?? null) === scenarioCase &&
        carried.params !== undefined
          ? { ...carried.params }
          : scenario !== null &&
              scenario === sessionScenario &&
              scenarioCase === sessionCase &&
              session?.intent?.params
            ? { ...session.intent.params }
            : {};
      return { file, id, scenario, scenarioCase, value };
    },
    {
      equal: (a, b) =>
        a.file === b.file &&
        a.id === b.id &&
        a.scenario === b.scenario &&
        a.scenarioCase === b.scenarioCase &&
        JSON.stringify(a.value) === JSON.stringify(b.value),
    },
  );
  protected readonly paramOverrides = linkedSignal<
    {
      file: string | null;
      id: string | null;
      scenario: string | null;
      scenarioCase: string | null;
      value: Readonly<Record<string, ScenarioParamValue>>;
    },
    Readonly<Record<string, ScenarioParamValue>>
  >({
    source: this.paramOverridesSource,
    computation: ({ value }) => value,
  });
  protected readonly paramInvalid = signal(false);

  /** ランチャーがシナリオ終了後の手動操作への引き継ぎに対応しているか。 */
  protected readonly handoverSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('scenario-handover'),
  );
  /** ランチャーがシナリオ外の追加信号記録（`record-signals`）に対応しているか。 */
  protected readonly recordSignalsSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('record-signals'),
  );
  /** ランチャーが手動実行でのタイムライン出力（`manual-timeline`）に対応しているか。 */
  protected readonly manualTimelineSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('manual-timeline'),
  );
  /** 記録する信号の選択欄を表示し、`intent.record` を送れるか。 */
  protected readonly recordSignalsAvailable = computed(
    () =>
      this.recordSignalsSupported() &&
      (this.chosenScenario() !== null || this.manualTimelineSupported()),
  );
  /** ランチャーが失敗後の追加記録（`record-after-fail`）に対応しているか。 */
  protected readonly recordAfterFailSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('record-after-fail'),
  );
  /** ランチャーが仮想時刻付き ECU イベント記録（`record-events`）に対応しているか。 */
  protected readonly recordEventsSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('record-events'),
  );
  /** 仮想時刻付き ECU イベント記録チェックボックスを表示し、`intent.record_events` を送れるか。 */
  protected readonly recordEventsAvailable = computed(
    () =>
      this.recordEventsSupported() &&
      (this.chosenScenario() !== null || this.manualTimelineSupported()),
  );
  /** ランチャーが仮想時刻付き CAN フレーム記録（`record-can`）に対応しているか。 */
  protected readonly recordCanSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('record-can'),
  );
  /** 仮想時刻付き CAN フレーム記録チェックボックスを表示し、`intent.record_can` を送れるか。 */
  protected readonly recordCanAvailable = computed(
    () =>
      this.recordCanSupported() &&
      (this.chosenScenario() !== null || this.manualTimelineSupported()),
  );
  /** 失敗後の追加記録（仮想時間 ms）。空欄で無効。 */
  private readonly recordAfterFailSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.id ?? null;
      const scenario = this.scenarioChoice();
      const carried = this.carriedFor(id);
      const sessionScenario =
        session && session.file === file
          ? (session.intent?.scenario ?? session.scenario?.file ?? null)
          : null;
      const sessionTail =
        scenario !== null && scenario === sessionScenario
          ? session?.intent?.record_after_fail_ms
          : undefined;
      return {
        file,
        id,
        scenario,
        value:
          scenario !== null &&
          carried &&
          carried.file === file &&
          carried.scenario === scenario &&
          carried.recordAfterFailMs !== undefined
            ? carried.recordAfterFailMs
            : sessionTail !== undefined
              ? String(sessionTail)
              : '',
      };
    },
    {
      equal: (a, b) =>
        a.file === b.file && a.id === b.id && a.scenario === b.scenario && a.value === b.value,
    },
  );
  protected readonly recordAfterFailMs = linkedSignal<
    { file: string | null; id: string | null; scenario: string | null; value: string },
    string
  >({
    source: this.recordAfterFailSource,
    computation: ({ value }) => value,
  });
  protected readonly recordAfterFailTouched = linkedSignal({
    source: this.recordAfterFailSource,
    computation: () => false,
  });
  protected readonly recordAfterFailInvalid = computed(() => {
    const value = this.recordAfterFailMs().trim();
    return (
      value !== '' &&
      (!/^\d+$/.test(value) || Number(value) < 1 || Number(value) > RECORD_AFTER_FAIL_MS_MAX)
    );
  });
  protected readonly sessionRecordAfterFailMs = computed<number | null>(
    () => this.session()?.intent?.record_after_fail_ms ?? null,
  );
  /** 追加で記録する信号（`<participant>.<variable>`）。既定は空（全信号の自動記録はしない）。 */
  private readonly recordedSignalsSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        id: session?.id ?? null,
        names:
          carried && carried.file === file && carried.record !== undefined
            ? carried.record
            : session && session.file === file && session.intent?.record
              ? session.intent.record
              : [],
      };
    },
    {
      equal: (a, b) =>
        a.file === b.file &&
        a.id === b.id &&
        a.names.length === b.names.length &&
        a.names.every((n, i) => n === b.names[i]),
    },
  );
  protected readonly recordedSignals = linkedSignal<
    { file: string | null; id: string | null; names: readonly string[] },
    ReadonlySet<string>
  >({
    source: this.recordedSignalsSource,
    computation: ({ names }) => new Set(names),
    equal: (a, b) => a.size === b.size && [...a].every((name) => b.has(name)),
  });
  protected readonly recordedSignalsList = computed(() => [...this.recordedSignals()].sort());
  protected readonly sessionRecordSignals = computed<readonly string[]>(
    () => this.session()?.intent?.record ?? [],
  );
  /** 仮想時刻付き ECU イベントを記録するか（既定はオフ。構成ファイル切替で初期化）。 */
  private readonly recordEventsSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        id: session?.id ?? null,
        value:
          carried && carried.file === file && carried.recordEvents !== undefined
            ? carried.recordEvents
            : session && session.file === file && session.intent?.record_events === true
              ? true
              : false,
      };
    },
    {
      equal: (a, b) => a.file === b.file && a.id === b.id && a.value === b.value,
    },
  );
  protected readonly recordEvents = linkedSignal<
    { file: string | null; id: string | null; value: boolean },
    boolean
  >({
    source: this.recordEventsSource,
    computation: ({ value }) => value,
  });
  protected readonly sessionRecordEvents = computed<boolean>(
    () => this.session()?.intent?.record_events === true,
  );
  /** 仮想時刻付き CAN フレームを記録するか（既定はオフ。構成ファイル切替で初期化）。 */
  private readonly recordCanSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const carried = this.carriedFor(session?.id ?? null);
      return {
        file,
        id: session?.id ?? null,
        value:
          carried && carried.file === file && carried.recordCan !== undefined
            ? carried.recordCan
            : session && session.file === file && session.intent?.record_can === true
              ? true
              : false,
      };
    },
    {
      equal: (a, b) => a.file === b.file && a.id === b.id && a.value === b.value,
    },
  );
  protected readonly recordCan = linkedSignal<
    { file: string | null; id: string | null; value: boolean },
    boolean
  >({
    source: this.recordCanSource,
    computation: ({ value }) => value,
  });
  protected readonly sessionRecordCan = computed<boolean>(
    () => this.session()?.intent?.record_can === true,
  );
  private readonly afterScenarioSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      const id = session?.id ?? null;
      const scenario = this.scenarioChoice();
      const carried = this.carriedFor(id);
      const sessionScenario =
        session && session.file === file
          ? (session.intent?.scenario ?? session.scenario?.file ?? null)
          : null;
      const sessionAfter =
        scenario !== null && scenario === sessionScenario
          ? session?.intent?.after_scenario
          : undefined;
      return {
        file,
        id,
        scenario,
        value:
          scenario !== null &&
          carried &&
          carried.file === file &&
          (carried.afterScenario === 'stop' || carried.afterScenario === 'pause')
            ? carried.afterScenario
            : sessionAfter === 'stop' || sessionAfter === 'pause'
              ? sessionAfter
              : ('stop' as AfterScenario),
      };
    },
    {
      equal: (a, b) =>
        a.file === b.file && a.id === b.id && a.scenario === b.scenario && a.value === b.value,
    },
  );
  protected readonly afterScenario = linkedSignal<
    { file: string | null; id: string | null; scenario: string | null; value: AfterScenario },
    AfterScenario
  >({
    source: this.afterScenarioSource,
    computation: ({ value }) => value,
  });
  /** シナリオ終了後に一時停止して手動操作へ引き継ぐ設定か。 */
  protected readonly pauseAfterScenario = computed(
    () =>
      this.chosenScenario() !== null &&
      this.handoverSupported() &&
      this.afterScenario() === 'pause',
  );

  /**
   * 次の実行で外につなぐ先（部品 <vsil-connect> が選ぶ。最初は空 = つながない）。同じページの間は実行の後も残り
   * （「もう一度実行」も同じ先につなぐ。開始のボタンの下の 1 行がそう言う）、構成ファイルを替えると消える。言語を
   * 変えて読み込み直したときは持ち越さない（黙って実機につないで始めない）。
   */
  private readonly connect = connectChoice();
  /** 送る、外につなぐ先（なければ null で、送らない）。 */
  private readonly connectToSend = computed(() =>
    Object.keys(this.connect()).length > 0 ? { ...this.connect() } : null,
  );
  private readonly list = new Intl.ListFormat(this.locale, { type: 'conjunction' });
  /**
   * 始めると外につなぐ先の 1 行（開始のボタンの下に出す。実行中（つないだ先は <vsil-outside> のカードが言う）と、
   * つながないときは null）。
   */
  protected readonly connectSummary = computed(() =>
    this.running() ? null : connectText(this.connect(), this.list),
  );

  /**
   * 次の実行の意図: 画面の選択（外す participant、自分で起動する participant、シナリオ、速度、止めて始める）と、部品が
   * 選んだ外につなぐ先。照合も開始も、これを正面の口に渡す（照合は外につないだ実行の時刻の決まり
   * （`outside-real-time` など）に速度と止めて始めるを使うので、合わない選択は始める前に所見で出る）。
   * - 速度の倍率の欄が読めない・範囲の外の間は、速度は最後に読めた値のまま（打っている途中の値は入らない。開始は
   *   blockedBy が止める）。
   * - シナリオを選んだら止めて始めない（ランチャーは 2 つを一緒には受けない。チェックボックスも押せなくしてある）。
   */
  private readonly intent = computed<Intent>(
    () => {
      const variant = this.variantChoice();
      const scenario = this.chosenScenario();
      const scenarioCase = scenario !== null ? this.caseChoice() : null;
      const overrides = this.paramOverrides();
      const paramKeys = scenario !== null ? Object.keys(overrides).sort() : [];
      const params: Record<string, ScenarioParamValue> | null =
        paramKeys.length > 0 ? Object.fromEntries(paramKeys.map((k) => [k, overrides[k]!])) : null;
      const pauseAfter =
        scenario !== null && this.handoverSupported() && this.afterScenario() === 'pause';
      const record = this.recordSignalsAvailable() ? this.recordedSignalsList() : [];
      const recordAfterFail =
        scenario !== null && this.recordAfterFailSupported() ? this.recordAfterFailMs().trim() : '';
      const recordEvents = this.recordEventsAvailable() && this.recordEvents();
      const recordCan = this.recordCanAvailable() && this.recordCan();
      const speed = this.speed();
      const paused = scenario === null && this.startPaused();
      const connect = this.connectToSend();
      const stopAt = pauseAfter ? '' : this.stopAt().trim();
      return {
        disable: [...this.disabled()].sort(),
        manual: this.manualToSend(),
        ...(variant !== null ? { variant } : {}),
        ...(scenario !== null ? { scenario } : {}),
        ...(scenarioCase !== null ? { case: scenarioCase } : {}),
        ...(params !== null ? { params } : {}),
        ...(pauseAfter ? { after_scenario: 'pause' as const } : {}),
        ...(record.length > 0 ? { record } : {}),
        ...(recordAfterFail !== '' && !this.recordAfterFailInvalid()
          ? { record_after_fail_ms: Number(recordAfterFail) }
          : {}),
        ...(recordEvents ? { record_events: true } : {}),
        ...(recordCan ? { record_can: true } : {}),
        ...(speed !== null ? { speed } : {}),
        ...(!pauseAfter && !this.stopAtInvalid() && stopAt !== ''
          ? { stop_at_ms: Number(stopAt) }
          : {}),
        ...(paused ? { start_paused: true } : {}),
        ...(connect !== null ? { connect } : {}),
      };
    },
    { equal: sameIntent },
  );

  /** 照合し直すよう頼んだ回数（同じ組み合わせでも照合し直すため）。 */
  private readonly rechecks = signal(0);
  /** 最後に送った照合の rechecks（それより後に頼んだ「照合し直す」は、新しく送る）。 */
  private checkedAgain = 0;

  /**
   * 選んだファイルを次の実行の意図で照合する。切り替えたら（速度を打ったら）CHECK_DELAY_MS 待ってから 1 回。正面の口が、
   * 前の照合の応答を待ってから送り、同じ意図の照合（部品 <vsil-connect> のもの）を分け合う。待っている間に意図が
   * 変わったら送らず（`signal`）、古い応答は捨てる。「照合し直す」の照合は、送っている最中の照合を分け合わない
   * （`fresh`。直したファイルを読む）。ランチャーが動いている間は照合しない。
   */
  protected readonly check = resource({
    params: () => {
      const file = this.selected();
      if (
        !this.app.hasValue() ||
        this.run.incompatible() ||
        this.auth.rejected() ||
        file === null ||
        this.running()
      )
        return undefined;
      return { file, intent: this.intent(), again: this.rechecks() };
    },
    loader: async ({ params, abortSignal }) => {
      await wait(CHECK_DELAY_MS, abortSignal);
      const fresh = params.again !== this.checkedAgain;
      this.checkedAgain = params.again;
      const result = await this.run.check(params.intent, params.file, {
        signal: abortSignal,
        fresh,
      });
      abortSignal.throwIfAborted();
      return result;
    },
  });

  /**
   * 選んだファイルの最後の照合の結果。照合し直している間も前の結果を残す（切り替えたスイッチが消えて、フォーカスを失わないように）。
   * ファイルを変えたら残さない。
   */
  protected readonly selectedCheck = linkedSignal<
    { file: string | null; value: CheckResult | undefined },
    CheckResult | null
  >({
    source: () => ({
      file: this.selected(),
      value: this.check.hasValue() ? this.check.value() : undefined,
    }),
    computation: (source, previous) =>
      source.value ?? (previous?.source.file === source.file ? previous.value : null),
  });

  /** 見せる照合の結果: 実行中はそのセッションの照合、それ以外は選んだファイルの照合。 */
  protected readonly shownCheck = computed<CheckResult | null>(() => {
    const session = this.session();
    if (this.running() && session) return session.check;
    return this.selectedCheck();
  });

  /** 選択中の構成ファイルが持つバリアント一覧。 */
  protected readonly availableVariants = computed<readonly string[]>(
    () => this.selectedCheck()?.variants ?? [],
  );
  /** 実行中のセッションのバリアント（なければ null）。 */
  protected readonly sessionVariant = computed<string | null>(() => {
    const session = this.session();
    return session?.intent?.variant ?? session?.check?.variant ?? null;
  });
  /** 選択中シナリオのケース一覧（バリアント制限 `only` の判定つき）。 */
  protected readonly caseOptions = computed<readonly CaseOption[]>(() =>
    caseOptionsFor(
      this.selectedCheck()?.scenario,
      this.variantChoice() ?? this.selectedCheck()?.variant ?? null,
    ),
  );
  /** 選択中シナリオの照合情報（パラメータ編集欄の入力）。 */
  protected readonly editableScenario = computed(() =>
    this.chosenScenario() !== null ? (this.selectedCheck()?.scenario ?? null) : null,
  );
  /** シナリオ終了後の「同じ条件で再実行」ボタンが押せるか。 */
  protected readonly rerunAllowed = computed(
    () =>
      !this.running() &&
      !this.run.starting() &&
      this.run.controls().start.ok &&
      this.session()?.scenario != null,
  );

  /** 始められない理由（なければ空）。ランチャーやサーバーが断る理由は、帯が正面の口の `controls.start` から出す。 */
  protected readonly blockedBy = computed<string[]>(() => {
    if (this.selected() === null) return [$localize`構成ファイルを選んでください。`];
    if (this.speedInvalid()) {
      return [$localize`速度の倍率が範囲の外です。`];
    }
    if (!this.pauseAfterScenario() && this.stopAtInvalid())
      return [$localize`停止時刻は 1 から 1000000000000 までの整数にしてください。`];
    if (
      this.chosenScenario() !== null &&
      this.recordAfterFailSupported() &&
      this.recordAfterFailInvalid()
    ) {
      return [$localize`失敗後の追加記録は 1 から 600000 までの整数にしてください。`];
    }
    if (this.chosenScenario() !== null && this.paramInvalid()) {
      return [$localize`パラメータの入力値を確認してください。`];
    }
    // 選んだシナリオを送れないうちは始めない（シナリオなしの実行が始まらないように）
    const scenario = this.scenarioChoice();
    if (scenario !== null && this.chosenScenario() === null) {
      if (this.scenarios.error()) {
        return [$localize`シナリオの一覧を読めないので、選んだシナリオで始められません。`];
      }
      if (!this.scenarios.hasValue() || this.scenarios.isLoading()) {
        return [$localize`シナリオの一覧を読んでいます。`];
      }
      return [$localize`選んだシナリオ ${scenario}:file: が一覧にありません。選び直してください。`];
    }
    if (this.check.isLoading()) return [$localize`照合しています。`];
    const error = this.check.error();
    if (error) return [$localize`照合できませんでした: ${errorMessage(error)}:reason:`];
    const check = this.shownCheck();
    if (!check) return [$localize`照合の結果を待っています。`];
    const guidance = checkSelectionGuidance(check);
    if (guidance === 'variant') return [$localize`バリアントを選んでください。`];
    if (guidance === 'case') return [$localize`シナリオのケースを選んでください。`];
    if (check.result !== 'passed') {
      const missing = check.scenario?.missing_params;
      if (missing && missing.length > 0) {
        return [
          $localize`未指定の必須パラメータ（${missing.join(', ')}:names:）を入力してください。`,
        ];
      }
      return [
        $localize`照合で誤りが見つかりました（誤り ${check.errors}:count: 件）。直してから始めてください。`,
      ];
    }
    return [];
  });

  protected readonly connectionText = computed(() => {
    // 版が合わずにつながないときは、つなぎ直していない（理由は別の帯）
    if (this.run.incompatible() !== null) return null;
    const state = this.run.connectionState();
    if (state !== 'open') {
      switch (this.run.tryAgainReason()) {
        case null:
          break;
        case 'too-many-connections':
          return $localize`この GUI サーバーにつないでいる画面が多すぎます。使わないタブを閉じてください。間を置いてつなぎ直しています…`;
        default:
          return $localize`サーバーが混んでいて、接続を後回しにしました。間を置いてつなぎ直しています…`;
      }
    }
    switch (state) {
      case 'connecting':
        return $localize`サーバーにつないでいます…`;
      case 'closed':
        return $localize`サーバーとの接続が切れました。つなぎ直しています…`;
      default:
        return null;
    }
  });

  /**
   * サーバーの知らせを code ごとにまとめる（同じ code は数を出す）。最後に届いた文を見せる。画面がほかの場所で
   * 日本語で説明している知らせ（異常終了のカードの no-result、状態の帯の launcher-silent）は `explained` に分け、
   * 「詳細」の中にたたむ。
   */
  protected readonly problemGroups = computed(() => {
    const session = this.session();
    if (!session) return { groups: [], explained: [], hidden: 0 };
    const groups = new Map<string, { code: string; message: string; count: number }>();
    for (const problem of session.problems) {
      // ランチャーの標準エラーは、別のまとまり（stderr）で出す
      if (problem.code === STDERR_CODE) continue;
      const group = groups.get(problem.code);
      if (group) {
        group.count++;
        group.message = problem.message;
      } else {
        groups.set(problem.code, { ...problem, count: 1 });
      }
    }
    const all = [...groups.values()].slice(0, MAX_PROBLEM_GROUPS);
    const explained = new Set(EXPLAINED_PROBLEMS);
    if (session.result || isRunActive(session)) explained.delete('no-result');
    return {
      groups: all.filter((g) => !explained.has(g.code)),
      explained: all.filter((g) => explained.has(g.code)),
      hidden:
        [...groups.values()].slice(MAX_PROBLEM_GROUPS).reduce((sum, g) => sum + g.count, 0) +
        session.problemsOmitted,
    };
  });

  /** ランチャーが標準エラーに書いた行（最初の行、落とした数、最後の行）。 */
  protected readonly stderr = computed(() => {
    const session = this.session();
    return session ? launcherStderr(session.problems) : null;
  });

  protected readonly hasProblems = computed(
    () => this.problemGroups().groups.length + this.problemGroups().explained.length > 0,
  );

  /** ランチャーが結果を出さずに終わった（participant の最後の状態は古いかもしれない）。 */
  protected readonly launcherGone = computed(() => this.run.status().run === 'crashed');

  /**
   * participant の終わりが、利用者や外から止めたことによるものか（色を弱める）: 止めるよう頼んだ（セッションの写し。
   * 正面の口は、実行が終わった後は言わない）か、結果の色が中立。
   */
  protected readonly interrupted = computed(() => {
    const session = this.session();
    const result = this.run.status().result;
    return (
      !!session &&
      (session.stopRequests > 0 ||
        (result !== null &&
          resultTone(result, cutShort(result, session.result?.result)) === 'neutral'))
    );
  });

  /** 自分で起動した participant のうち、終わった後も残っているとランチャーがログで知らせたもの（名前から行へ）。 */
  protected readonly manualStays = computed(() => {
    const stays = new Map<string, string>();
    const session = this.session();
    if (!session?.result) return stays;
    const manual = session.participants.filter((p) => p.manual).map((p) => p.name);
    for (const log of session.logs) {
      if (log.source !== LAUNCHER_SOURCE) continue;
      const name = manual.find((n) => log.line.startsWith(n + MANUAL_STAYS));
      if (name !== undefined) stays.set(name, log.line);
    }
    return stays;
  });

  /** ログで絞る出どころ（「ランチャーのログを見る」で選ぶ）。 */
  protected readonly logSource = signal(this.carried?.logSource ?? '');
  protected readonly copyLabel = $localize`選べる範囲のパスをコピー`;

  constructor() {
    this.language.keep(
      'run',
      (): RunChoices => ({
        session: this.session()?.id ?? null,
        chosen: this.chosen(),
        file: this.selected(),
        disable: [...this.disabled()],
        manual: [...this.manual()],
        record: this.recordedSignalsList(),
        recordAfterFailMs: this.recordAfterFailMs(),
        recordEvents: this.recordEvents(),
        recordCan: this.recordCan(),
        speed: this.speed(),
        stopAt: this.stopAt(),
        stopAtEdited: this.stopAtEdited(),
        stopSession: this.session()?.file === this.selected() ? (this.session()?.id ?? null) : null,
        variant: this.variantChoice(),
        scenario: this.scenarioChoice(),
        case: this.caseChoice(),
        ...(Object.keys(this.paramOverrides()).length > 0
          ? { params: { ...this.paramOverrides() } }
          : {}),
        afterScenario: this.afterScenario(),
        startPaused: this.startPaused(),
        logSource: this.logSource(),
      }),
      inject(DestroyRef),
    );
    // 画面の欄を pageIntent に書く（外につなぐ先は部品が書く）。走行中は書かない（部品は隠れていて、照合もしない）。
    // ページを離れたら消す（ほかのページや次の読み込みに残さない）。構成ファイルを替えたら、欄はそのファイルの選択に
    // 戻り、それを書く
    const own = computed(
      () => {
        const {
          disable,
          manual,
          variant,
          scenario,
          case: scenarioCase,
          params,
          after_scenario,
          record,
          record_after_fail_ms,
          record_events,
          record_can,
          speed,
          start_paused,
          stop_at_ms,
        } = this.intent();
        return {
          disable,
          manual,
          variant,
          scenario,
          case: scenarioCase,
          params,
          after_scenario,
          record,
          record_after_fail_ms,
          record_events,
          record_can,
          speed,
          start_paused,
          stop_at_ms,
        };
      },
      { equal: sameIntent },
    );
    effect(() => {
      if (!this.running()) pageIntent.update(own());
    });
    inject(DestroyRef).onDestroy(() =>
      pageIntent.update({
        disable: undefined,
        manual: undefined,
        variant: undefined,
        scenario: undefined,
        case: undefined,
        params: undefined,
        after_scenario: undefined,
        record: undefined,
        record_after_fail_ms: undefined,
        record_events: undefined,
        record_can: undefined,
        speed: undefined,
        start_paused: undefined,
        stop_at_ms: undefined,
      }),
    );
  }

  /** 持ち越した選択。持ち越したときと同じセッションの間だけ使う。 */
  private carriedFor(session: string | null): RunChoices | null {
    return this.carried && this.carried.session === session ? this.carried : null;
  }

  private carriedStopFor(session: string | null): RunChoices | null {
    return this.carried &&
      (this.carried.stopSession === undefined ? this.carried.session : this.carried.stopSession) ===
        session
      ? this.carried
      : null;
  }

  protected selectFile(file: string): void {
    if (file !== this.selected()) {
      this.paramInvalid.set(false);
    }
    this.chosen.set(file);
  }

  protected start(): void {
    const file = this.selected();
    if (file === null) return;
    this.document.querySelector('mat-sidenav-content')?.scrollTo(0, 0);
    this.run.start(this.intent(), file);
  }

  protected selectVariant(variant: string | null): void {
    this.variantChoice.set(variant);
    const currentCase = this.caseChoice();
    if (currentCase !== null) {
      const options = caseOptionsFor(this.selectedCheck()?.scenario, variant);
      if (options.some((opt) => opt.name === currentCase && opt.disabled)) {
        this.caseChoice.set(null);
        this.paramOverrides.set({});
        this.paramInvalid.set(false);
      }
    }
  }

  protected selectScenario(scenario: string | null): void {
    this.scenarioChoice.set(scenario);
    this.caseChoice.set(null);
    this.paramOverrides.set({});
    this.paramInvalid.set(false);
    if (scenario === null) this.afterScenario.set('stop');
  }

  protected selectCase(scenarioCase: string | null): void {
    this.caseChoice.set(scenarioCase);
    this.paramOverrides.set({});
    this.paramInvalid.set(false);
  }

  protected selectAfterScenario(value: AfterScenario): void {
    this.afterScenario.set(value);
  }

  protected caseDisabledReason(option: CaseOption): string | null {
    if (!option.disabled || !option.only) return null;
    return $localize`バリアント ${option.only.join(', ')}:variants: のみ対象`;
  }

  /**
   * 終了したセッションと同じ条件で単独再実行する。
   * サーバー側の `session.intent` をそのまま通常の `start(intent, file)`（`POST /api/run`）へ渡すことで、
   * `1 POST /api/run = 1 Session = 1 Timeline`、外部接続の TOCTOU 検査、ホットスタンバイ再利用をそのまま保つ。
   */
  protected rerunSession(): void {
    const session = this.session();
    if (!session || !this.rerunAllowed()) return;
    const intent: Intent = session.intent
      ? { ...session.intent }
      : this.fallbackSessionIntent(session);
    this.document.querySelector('mat-sidenav-content')?.scrollTo(0, 0);
    this.run.start(intent, session.file);
  }

  private fallbackSessionIntent(session: SessionView): Intent {
    const variant = session.check?.variant ?? undefined;
    const scenario = session.scenario?.file ?? undefined;
    const scenarioCase = session.scenario?.case ?? undefined;
    return {
      disable: [...session.disable],
      manual: [...session.manual],
      ...(variant ? { variant } : {}),
      ...(scenario ? { scenario } : {}),
      ...(scenario && scenarioCase ? { case: scenarioCase } : {}),
      ...(session.speed !== null ? { speed: session.speed } : {}),
      ...(session.startPaused ? { start_paused: true } : {}),
    };
  }

  /** 手動実行のタイムラインを別タブの時間の軸ページで開く URL。 */
  protected manualTimelineHref(url: string): string {
    return timelinePageUrl(url, this.language.current);
  }

  /** この実行の操作をシナリオとして保存できるか（変数を読み書きできる実行で、シナリオの実行でない）。 */
  protected readonly recordable = computed(() => {
    const session = this.session();
    return !!session && session.scenario === null && !!this.info.variables()?.available;
  });

  protected recheck(): void {
    this.rechecks.update((n) => n + 1);
  }

  /** 結果（または異常終了）のカードへ移る。 */
  protected showDetails(): void {
    this.reveal('run-result');
  }

  /** ランチャー自身のログに絞って、ログへ移る。 */
  protected showLauncherLog(): void {
    this.logSource.set(LAUNCHER_SOURCE);
    this.reveal('run-log');
  }

  protected showProblems(): void {
    this.reveal('run-problems');
  }

  protected showStderr(): void {
    this.reveal('run-stderr');
  }

  protected copyRoot(path: string): void {
    void navigator.clipboard?.writeText(path).then(() => this.copiedPath.set(true));
  }

  /**
   * 見出しへフォーカスを移す（スクロールもする）。上に貼り付いた帯の高さは幅と中身で変わるので、見出しを窓の
   * 真ん中へ送る（帯に隠れない）。
   */
  private reveal(id: string): void {
    const heading = this.document.getElementById(id);
    heading?.focus({ preventScroll: true });
    heading?.scrollIntoView({ block: 'center' });
  }
}
