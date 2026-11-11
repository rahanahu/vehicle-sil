import { httpResource } from '@angular/common/http';
import { Component, computed, inject, linkedSignal, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCardModule } from '@angular/material/card';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatSelectModule } from '@angular/material/select';
import {
  CheckResult,
  Speed,
  errorMessage,
  readAppInfo,
  readCheckResponse,
  readConfigList,
} from '../core/api';
import { AuthToken } from '../core/auth/auth-token';
import { CheckPanel } from './check-panel';
import { FindingsList } from './findings-list';
import { LogView } from './log-view';
import { ParticipantStatus } from './participant-status';
import { ResultPanel } from './result-panel';
import { RunControls } from './run-controls';
import { RunSession } from './run-session';

/**
 * 実行の画面: 構成ファイルを選び、照合の結果を見て participant を外し、始めて、経過とログを見て、結果を見る。
 *
 * - 実行の状態はサーバーにあり、RunSession が WebSocket で映す（この画面と同じ寿命）。
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
    RunControls,
  ],
  providers: [RunSession],
  templateUrl: './run-page.html',
  styleUrl: './run-page.css',
})
export class RunPage {
  protected readonly auth = inject(AuthToken);
  protected readonly run = inject(RunSession);

  protected readonly app = httpResource(() => (this.auth.token() ? 'api/app' : undefined), {
    parse: readAppInfo,
  });
  protected readonly configs = httpResource(() => (this.auth.token() ? 'api/configs' : undefined), {
    parse: readConfigList,
  });

  protected readonly session = this.run.session;
  protected readonly active = this.run.active;

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

  /** セッションのファイルを選んでいるときの、そのセッションで外した participant（「もう一度実行」を同じ組み合わせにする）。 */
  private readonly sessionDisable = computed(() => {
    const session = this.session();
    return session && session.file === this.selected() ? session.disable.join('\n') : '';
  });
  /** 外す participant。ファイルを変えたら、そのファイルのセッションで外したもの（なければ空）に戻す。 */
  protected readonly disabled = linkedSignal<string, ReadonlySet<string>>({
    source: () => `${this.selected()}\n\n${this.sessionDisable()}`,
    computation: () =>
      new Set(
        this.sessionDisable()
          .split('\n')
          .filter((name) => name !== ''),
      ),
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

  /** 選んだファイルと外す participant で照合する（変えるたびに照合し直す。前の要求は取り消す）。実行中は照合しない。 */
  protected readonly check = httpResource(
    () => {
      const file = this.selected();
      if (!this.auth.token() || file === null || this.active()) return undefined;
      return {
        url: 'api/check',
        method: 'POST',
        body: { file, disable: [...this.disabled()].sort() },
      };
    },
    { parse: readCheckResponse },
  );

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
    if (this.active() && session) return session.check;
    return this.lastCheck();
  });

  /** 始められない理由（なければ空）。 */
  protected readonly blockedBy = computed<string[]>(() => {
    if (this.app.hasValue() && !this.app.value().launcher.usable) {
      return [$localize`ランチャーがこの画面に対応していません。`];
    }
    if (this.selected() === null) return [$localize`構成ファイルを選んでください。`];
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
    switch (this.run.connectionState()) {
      case 'connecting':
        return $localize`サーバーにつないでいます…`;
      case 'closed':
        return $localize`サーバーとの接続が切れました。つなぎ直しています…`;
      default:
        return null;
    }
  });

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

  protected stop(): void {
    this.run.stop();
  }

  protected recheck(): void {
    this.check.reload();
  }
}
