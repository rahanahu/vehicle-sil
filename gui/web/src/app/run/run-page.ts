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
import { MatSelectModule } from '@angular/material/select';
import { CheckResult, Intent, Speed, errorMessage } from '@vehicle-sil/gui-client';
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
import { RecordingPanel } from './recording-panel';
import { ResultPanel } from './result-panel';
import { RunSession } from './run-session';
import { RunStatus } from './run-status';
import { ScenarioPanel } from './scenario-panel';
import { SpeedPicker } from './speed-picker';
import { TimeButtons } from './time-buttons';
import { TimePanel } from './time-panel';

/** 「使う」を切り替えてから照合を頼むまでの間（ミリ秒）。続けて切り替えたら、最後の組み合わせで 1 回だけ照合する。 */
export const CHECK_DELAY_MS = 300;

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
  session: number | null;
  chosen: string | null;
  /** 外す participant を選んだときのファイル。 */
  file: string | null;
  disable: string[];
  /** 自分で起動する participant（古い持ち越しにはない）。 */
  manual?: string[];
  speed: Speed;
  stopAt?: string;
  stopAtEdited?: boolean;
  stopSession?: number | null;
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
    !(v['session'] === null || typeof v['session'] === 'number') ||
    !(v['chosen'] === null || isText(v['chosen'])) ||
    !(v['file'] === null || isText(v['file'])) ||
    !Array.isArray(v['disable']) ||
    !v['disable'].every(isText) ||
    !(v['manual'] === undefined || (Array.isArray(v['manual']) && v['manual'].every(isText))) ||
    !speedOk ||
    !(v['startPaused'] === undefined || typeof v['startPaused'] === 'boolean') ||
    !(v['stopAt'] === undefined || typeof v['stopAt'] === 'string') ||
    !(v['stopAtEdited'] === undefined || typeof v['stopAtEdited'] === 'boolean') ||
    !(
      v['stopSession'] === undefined ||
      v['stopSession'] === null ||
      typeof v['stopSession'] === 'number'
    ) ||
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
    MatSelectModule,
    CheckPanel,
    FindingsList,
    LogView,
    OutsideChoice,
    OutsideRows,
    ParticipantStatus,
    RecordingPanel,
    ResultPanel,
    RunStatus,
    ScenarioPanel,
    SpeedPicker,
    TimeButtons,
    TimePanel,
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
    params: () => this.auth.token() ?? undefined,
    loader: () => this.info.configs(),
  });

  /**
   * WebSocket の握手で断られたとき、その理由（ブラウザからは見えない）を api/app で確かめる。
   * 断られるたびに 1 回呼ぶ。トークンの誤りは interceptor が「断られた」にする。
   */
  protected readonly handshake = resource({
    params: () => {
      const failures = this.run.handshakeFailures();
      return failures > 0 && this.auth.token() ? failures : undefined;
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
    return !!session && !this.running() && this.selected() !== session.file;
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
  protected readonly timeButtonsShown = computed(() => {
    const session = this.session();
    if (this.run.starting()) return true;
    return !!session && session.launcher.running && !session.result;
  });
  /** 「時刻」のカードを出すか: 実行中（結果が出るかランチャーが終わるまで）。 */
  protected readonly timeCardShown = computed(() => {
    const session = this.session();
    return !!session && session.launcher.running && !session.result;
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
    { file: string | null; id: number | null; value: string },
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
        !this.auth.token() ||
        file === null ||
        this.running()
      )
        return undefined;
      return { file };
    },
    loader: ({ params }) => this.info.scenarios(params.file),
  });
  /**
   * 選んだシナリオ。ファイルか実行が替わったら、その実行のもの（同じファイルのとき）か、なしに戻す（実行の知らせの
   * たびには戻さない）。
   */
  private readonly scenarioSource = computed(
    () => {
      const file = this.selected();
      const session = this.session();
      return {
        file,
        id: session?.id ?? null,
        scenario: session && session.file === file ? (session.scenario?.file ?? null) : null,
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
      const scenario = this.chosenScenario();
      const speed = this.speed();
      const paused = scenario === null && this.startPaused();
      const connect = this.connectToSend();
      const stopAt = this.stopAt().trim();
      return {
        disable: [...this.disabled()].sort(),
        manual: this.manualToSend(),
        ...(scenario !== null ? { scenario } : {}),
        ...(speed !== null ? { speed } : {}),
        ...(!this.stopAtInvalid() && stopAt !== '' ? { stop_at_ms: Number(stopAt) } : {}),
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
        !this.auth.token() ||
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

  /** 始められない理由（なければ空）。ランチャーやサーバーが断る理由は、帯が正面の口の `controls.start` から出す。 */
  protected readonly blockedBy = computed<string[]>(() => {
    if (this.selected() === null) return [$localize`構成ファイルを選んでください。`];
    if (this.speedInvalid()) {
      return [$localize`速度の倍率が範囲の外です。`];
    }
    if (this.stopAtInvalid())
      return [$localize`停止時刻は 1 から 1000000000000 までの整数にしてください。`];
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
    if (check.result !== 'passed') {
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
    if (session.result || session.launcher.running) explained.delete('no-result');
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
        speed: this.speed(),
        stopAt: this.stopAt(),
        stopAtEdited: this.stopAtEdited(),
        stopSession: this.session()?.file === this.selected() ? (this.session()?.id ?? null) : null,
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
        const { disable, manual, scenario, speed, start_paused, stop_at_ms } = this.intent();
        return { disable, manual, scenario, speed, start_paused, stop_at_ms };
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
        scenario: undefined,
        speed: undefined,
        start_paused: undefined,
        stop_at_ms: undefined,
      }),
    );
  }

  /** 持ち越した選択。持ち越したときと同じセッションの間だけ使う。 */
  private carriedFor(session: number | null): RunChoices | null {
    return this.carried && this.carried.session === session ? this.carried : null;
  }

  private carriedStopFor(session: number | null): RunChoices | null {
    return this.carried &&
      (this.carried.stopSession === undefined ? this.carried.session : this.carried.stopSession) ===
        session
      ? this.carried
      : null;
  }

  protected selectFile(file: string): void {
    this.chosen.set(file);
  }

  protected start(): void {
    const file = this.selected();
    if (file === null) return;
    this.run.start(this.intent(), file);
  }

  protected selectScenario(scenario: string | null): void {
    this.scenarioChoice.set(scenario);
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
