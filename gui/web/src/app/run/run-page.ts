import { HttpClient, HttpErrorResponse, httpResource } from '@angular/common/http';
import {
  Component,
  DOCUMENT,
  computed,
  inject,
  linkedSignal,
  resource,
  signal,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCardModule } from '@angular/material/card';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatSelectModule } from '@angular/material/select';
import { firstValueFrom } from 'rxjs';
import {
  CheckResult,
  Speed,
  errorMessage,
  readAppInfo,
  readCheckResponse,
  readConfigList,
} from '../core/api';
import { AuthToken } from '../core/auth/auth-token';
import { shortPath } from '../core/format/short-path';
import { CheckPanel } from './check-panel';
import { FindingsList } from './findings-list';
import { STDERR_CODE, launcherStderr } from './launcher-stderr';
import { LogView } from './log-view';
import { ParticipantStatus } from './participant-status';
import { ResultPanel } from './result-panel';
import { RunSession } from './run-session';
import { RunStatus } from './run-status';
import { SpeedPicker } from './speed-picker';

/** 「使う」を切り替えてから照合を頼むまでの間（ミリ秒）。続けて切り替えたら、最後の組み合わせで 1 回だけ照合する。 */
export const CHECK_DELAY_MS = 300;

/** 画面に出すサーバーの知らせの種類（code）の数。 */
const MAX_PROBLEM_GROUPS = 50;
/** 画面がほかの場所で日本語で説明している知らせ（no-result は異常終了のカード、launcher-silent は状態の帯）。 */
const EXPLAINED_PROBLEMS = ['no-result', 'launcher-silent'];

/** ランチャー自身のログの出どころ。 */
const LAUNCHER_SOURCE = 'vehicle_sil_launch';

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
 */
@Component({
  selector: 'app-run-page',
  imports: [
    MatButtonModule,
    MatCardModule,
    MatFormFieldModule,
    MatSelectModule,
    CheckPanel,
    FindingsList,
    LogView,
    ParticipantStatus,
    ResultPanel,
    RunStatus,
    SpeedPicker,
  ],
  providers: [RunSession],
  templateUrl: './run-page.html',
  styleUrl: './run-page.css',
})
export class RunPage {
  private readonly http = inject(HttpClient);
  private readonly document = inject(DOCUMENT);
  protected readonly auth = inject(AuthToken);
  protected readonly run = inject(RunSession);

  protected readonly app = httpResource(() => (this.auth.token() ? 'api/app' : undefined), {
    parse: readAppInfo,
  });
  protected readonly configs = httpResource(() => (this.auth.token() ? 'api/configs' : undefined), {
    parse: readConfigList,
  });

  /**
   * WebSocket の握手で断られたとき、その理由（ブラウザからは見えない）を api/app で確かめる。
   * 断られるたびに 1 回呼ぶ。トークンの誤りは interceptor が「断られた」にする。
   */
  protected readonly handshake = httpResource(
    () => {
      const failures = this.run.handshakeFailures();
      return failures > 0 && this.auth.token() ? { url: 'api/app' } : undefined;
    },
    { parse: readAppInfo },
  );
  protected readonly handshakeError = computed(() => {
    if (this.run.connectionState() === 'open') return null;
    const error = this.handshake.error();
    return error ? errorMessage(error) : null;
  });
  /**
   * HTTP でサーバーに届くか。WebSocket が切れて、理由を確かめる api/app も届かなかった（状態 0）ときだけ偽。
   * WebSocket だけが切れているなら、停止は HTTP で頼める。
   */
  protected readonly reachable = computed(() => {
    if (this.run.connectionState() === 'open') return true;
    const error = this.handshake.error();
    return !(error instanceof HttpErrorResponse && error.status === 0);
  });

  protected readonly session = this.run.session;
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
  private readonly chosen = signal<string | null>(null);
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
      return { file, names: session && session.file === file ? session.disable : [] };
    },
    computation: ({ names }) => new Set(names),
    equal: (a, b) => a.size === b.size && [...a].every((name) => b.has(name)),
  });

  /** ランチャーが速度を変えられるか（できないときは欄を出さず、速度を送らない）。 */
  protected readonly speedSupported = computed(
    () => this.app.hasValue() && this.app.value().launcher.features.includes('speed'),
  );
  protected readonly speedRange = computed(() =>
    this.app.hasValue() ? this.app.value().limits : { speed_min: 0.01, speed_max: 1000 },
  );
  /** 速度。前の実行のもの（なければ構成ファイルのとおり）から始める。 */
  protected readonly speed = linkedSignal<Speed>(() => this.session()?.speed ?? null);
  /** 速度の倍率の欄が範囲の外。 */
  protected readonly speedInvalid = signal(false);

  /** 照合し直すよう頼んだ回数（同じ組み合わせでも照合し直すため）。 */
  private readonly rechecks = signal(0);
  /** 送った照合の要求の終わり。次の照合は、前の照合の応答を待ってから送る（サーバーで重ならない）。 */
  private checkChain: Promise<unknown> = Promise.resolve();

  /**
   * 選んだファイルと外す participant で照合する。切り替えたら CHECK_DELAY_MS 待ってから 1 回、前の照合の応答を
   * 待ってから送る。途中で組み合わせが変わったら、古い応答は捨てる。ランチャーが動いている間は照合しない。
   */
  protected readonly check = resource({
    params: () => {
      const file = this.selected();
      if (!this.auth.token() || file === null || this.running()) return undefined;
      return { file, disable: [...this.disabled()].sort(), again: this.rechecks() };
    },
    loader: async ({ params, abortSignal }) => {
      await wait(CHECK_DELAY_MS, abortSignal);
      const body = { file: params.file, disable: params.disable };
      const request = this.checkChain.then(() => {
        // 待っている間に組み合わせが変わったら送らない
        abortSignal.throwIfAborted();
        return firstValueFrom(this.http.post('api/check', body));
      });
      this.checkChain = request.catch(() => undefined);
      const response = await request;
      abortSignal.throwIfAborted();
      return readCheckResponse(response);
    },
  });

  /**
   * 選んだファイルの最後の照合の結果。照合し直している間も前の結果を残す（切り替えたスイッチが消えて、フォーカスを失わないように）。
   * ファイルを変えたら残さない。
   */
  private readonly lastCheck = linkedSignal<
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
    return this.lastCheck();
  });

  /** 始められない理由（なければ空）。 */
  protected readonly blockedBy = computed<string[]>(() => {
    if (this.app.hasValue() && !this.app.value().launcher.usable) {
      return [$localize`ランチャーがこの画面に対応していません。`];
    }
    if (this.selected() === null) return [$localize`構成ファイルを選んでください。`];
    if (this.speedSupported() && this.speedInvalid()) {
      return [$localize`速度の倍率が範囲の外です。`];
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
  protected readonly launcherGone = computed(() => this.run.outcome() === 'crashed');

  /** participant の終わりが、利用者が止めたことによるものか（色を弱める）。 */
  protected readonly interrupted = computed(() => {
    const session = this.session();
    return !!session && (session.stopRequests > 0 || session.result?.result === 'interrupted');
  });

  /** ログで絞る出どころ（「ランチャーのログを見る」で選ぶ）。 */
  protected readonly logSource = signal('');
  protected readonly copyLabel = $localize`選べる範囲のパスをコピー`;

  protected selectFile(file: string): void {
    this.chosen.set(file);
  }

  protected start(): void {
    const file = this.selected();
    if (file === null) return;
    this.run.start({
      file,
      disable: [...this.disabled()].sort(),
      speed: this.speedSupported() ? this.speed() : null,
      start_paused: false,
    });
  }

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
