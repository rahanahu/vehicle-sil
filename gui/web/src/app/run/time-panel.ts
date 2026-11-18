import { Component, computed, inject, linkedSignal, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { Speed } from '@vehicle-sil/gui-client';
import { MeasurePipe } from '../core/format/measure-pipe';
import { numberText } from '../core/format/number-text';
import { SimulationTime } from '../core/session/simulation-time';
import { controlLine, latenessText, reasonLine, speedLabel } from './labels';
import { SpeedPicker } from './speed-picker';

/**
 * 時刻のカードに見せない理由の code（説明としてボタンには残す）: 接続（`offline`、`connecting`）は帯の 1 行が言う。
 * 部品の QUIET（not-running、offline）より多いのは、カードがいつも帯の隣にあるため。
 */
const BAND_SAYS = new Set(['offline', 'connecting']);
/**
 * 一時停止と再開の理由のうち、時刻のカードに出さない code: 接続のほか、押しても何も変わらない（`paused`、
 * `not-paused`: 状態の欄が言う）、実行がない（`not-running`）、答えを待っている（`busy`: 帯のボタンの説明が言う）。
 */
const SAID_ELSEWHERE = new Set([...BAND_SAYS, 'paused', 'not-paused', 'not-running', 'busy']);

/** ステップ送りの量の最初の値（ミリ秒）。 */
export const DEFAULT_STEP_MS = 100;

/**
 * 実行中の時刻: 仮想時間、速さ、実際の速さ、一時停止しているか、遅れの数と、ステップ送り、速さの変更。
 * 一時停止と再開は状態の帯にある（TimeButtons）。状態はすべて SimulationTime の signal から読む。操作の可否、理由、
 * 注意、範囲は正面の口の `controls` を写す（この画面は規則を持たない）。
 */
@Component({
  selector: 'app-time-panel',
  imports: [MatButtonModule, MatFormFieldModule, MatInputModule, MeasurePipe, SpeedPicker],
  template: `
    <dl class="facts">
      <div>
        <dt i18n>仮想時刻</dt>
        <dd data-testid="time-sim">
          @if (simSeconds() !== null) {
            <ng-container i18n>{{ simSeconds() | measure: 3 }} s</ng-container>
          } @else {
            —
          }
        </dd>
      </div>
      <div>
        <dt i18n>速度</dt>
        <dd data-testid="time-speed">{{ speedText() }}</dd>
      </div>
      <div>
        <dt i18n>実際の速さ</dt>
        <dd data-testid="time-actual">
          @if (time.actualSpeed() !== null) {
            <ng-container i18n>{{ time.actualSpeed() | measure: 2 }} 倍</ng-container>
          } @else {
            —
          }
        </dd>
      </div>
      <div>
        <dt i18n>状態</dt>
        <dd data-testid="time-state">{{ stateText() }}</dd>
      </div>
    </dl>
    @if (time.paced() || time.behind()) {
      @if (time.lateness(); as late) {
        <dl class="facts lateness" data-testid="time-lateness">
          <div>
            <dt i18n>遅れた刻み</dt>
            <dd>{{ late.lateSteps }}</dd>
          </div>
          <div>
            <dt i18n>最大の遅れ</dt>
            <dd i18n>{{ late.maxLateMs }} ms</dd>
          </div>
          <div>
            <dt i18n>ずれの合計</dt>
            <dd i18n>{{ late.slippedMs }} ms</dd>
          </div>
          <div>
            <dt i18n>いまの遅れ</dt>
            <dd i18n>{{ late.lateMs }} ms</dd>
          </div>
        </dl>
      }
    }
    <!-- 読み上げない（遅れを読み上げるのは状態の帯だけ） -->
    <p class="late" data-testid="time-late" [class.shown]="lateText() !== null">
      @if (lateText(); as text) {
        <span class="mark" aria-hidden="true">⚠</span> {{ text }}
      }
    </p>

    @if (time.supported()) {
      <!-- 一時停止と再開（ボタンは状態の帯）の、見える説明。押しても何も変わらないことと、実行がないことは言わない -->
      @if (pauseLines().length > 0) {
        <p class="hint" id="time-pause-hint" data-testid="pause-hint">
          @for (line of pauseLines(); track line.text) {
            <span [attr.lang]="line.english ? 'en' : null">{{ line.text }}</span>&ngsp;
          }
        </p>
      }
      <!-- Enter は form の暗黙の送信で送る: 日本語入力の変換を確定する Enter では、ブラウザーは送らない -->
      <form class="row" (submit)="sendStep($event)">
        <mat-form-field class="ms" subscriptSizing="dynamic">
          <mat-label i18n>ステップ送りの量（ms）</mat-label>
          <input
            matInput
            type="text"
            inputmode="numeric"
            autocomplete="off"
            data-testid="step-ms"
            [value]="stepText()"
            [attr.aria-invalid]="shownStepError() !== null"
            [attr.aria-describedby]="shownStepError() !== null ? 'step-ms-error' : null"
            #stepInput
            (input)="typeStep(stepInput.value)"
            (blur)="stepChecked.set(true)"
          />
        </mat-form-field>
        <button
          matButton="tonal"
          type="submit"
          data-testid="step"
          [disabled]="!canStep()"
          disabledInteractive
          [attr.aria-describedby]="stepWhy() ? 'time-step-why' : null"
          i18n
        >
          ステップ送り
        </button>
      </form>
      <!-- 誤りは、欄を離れたときか送ったときに出す（打っている間は消すだけ） -->
      <p
        class="error"
        id="step-ms-error"
        role="status"
        data-testid="step-ms-error"
        [class.shown]="shownStepError() !== null"
      >
        {{ shownStepError() }}
      </p>
      <p class="hint" id="time-step-why" data-testid="step-hint">
        @if (stepWhy(); as why) {
          <span [class.quiet]="!why.shown" [attr.lang]="why.english ? 'en' : null">{{
            why.text
          }}</span>&ngsp;
        }
        {{ stepMoved() }}
      </p>

      <div class="row">
        <app-speed-picker
          [withFile]="false"
          [label]="speedLabelText"
          [min]="controls().speed.min"
          [max]="controls().speed.max"
          [fastest]="controls().speed.fastest"
          [locked]="speedLocked()"
          [(speed)]="newSpeed"
          [(invalid)]="newSpeedInvalid"
        />
        <button
          matButton="tonal"
          type="button"
          data-testid="set-speed"
          [disabled]="!canSetSpeed()"
          disabledInteractive
          [attr.aria-describedby]="speedWhy() ? 'time-speed-why' : null"
          (click)="setSpeed()"
          i18n
        >
          速度を変える
        </button>
      </div>
      @if (speedWhy(); as why) {
        <p class="hint" id="time-speed-why" data-testid="speed-hint">
          <span [class.quiet]="!why.shown" [attr.lang]="why.english ? 'en' : null">{{
            why.text
          }}</span>
        </p>
      }
    }

    <p class="error" role="alert" data-testid="time-refused" [class.shown]="refusal() !== null">
      @if (refusal(); as reason) {
        @if (reason.english) {
          <ng-container i18n>命令を受け付けられませんでした:</ng-container>
          <span lang="en">{{ reason.text }}</span>
        } @else {
          {{ reason.text }}
        }
      }
    </p>
  `,
  styles: `
    :host {
      display: grid;
      gap: var(--app-space-2);
      min-inline-size: 0;
    }
    .facts {
      display: flex;
      flex-wrap: wrap;
      gap: 0 var(--app-space-4);
      margin: 0;
    }
    .facts div {
      display: flex;
      gap: var(--app-space-2);
    }
    .facts dt {
      color: var(--mat-sys-on-surface-variant);
    }
    .facts dd {
      margin: 0;
      font-variant-numeric: tabular-nums;
    }
    .row {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--app-space-2);
      min-inline-size: 0;
    }
    .ms {
      inline-size: 12em;
      max-inline-size: 100%;
    }
    p {
      margin: 0;
    }
    .hint {
      color: var(--mat-sys-on-surface-variant);
    }
    .late,
    .error {
      color: var(--mat-sys-error);
      overflow-wrap: anywhere;
    }
    .late:not(.shown) {
      display: none;
    }
    /* ボタンの説明には残し、見た目からは消す文（帯か、カードの上の行が同じことを言う） */
    .quiet {
      position: absolute;
      inline-size: 1px;
      block-size: 1px;
      overflow: hidden;
      clip-path: inset(50%);
      white-space: nowrap;
    }
    /* 読み上げの領域は中身がない間も置き、見た目からは消す */
    .error:not(.shown) {
      position: absolute;
      inline-size: 1px;
      block-size: 1px;
      overflow: hidden;
      clip-path: inset(50%);
    }
  `,
})
export class TimePanel {
  protected readonly time = inject(SimulationTime);
  /** 押せるか、押せない理由、範囲（正面の口が言う）。 */
  protected readonly controls = this.time.controls;
  protected readonly speedLabelText = $localize`新しい速度`;

  protected readonly simSeconds = computed(() => {
    const ms = this.time.simMs();
    return ms === null ? null : ms / 1000;
  });
  protected readonly speedText = computed(() => speedLabel(this.time.speed()));
  protected readonly stateText = computed(() => {
    const paused = this.time.paused();
    if (paused === null) return '—';
    return paused ? $localize`一時停止中` : $localize`動いている`;
  });
  /** 遅れの文（許容を超えて遅れた刻みがあるとき。最速に変えた後も、それまでの遅れは出す）。 */
  protected readonly lateText = computed(() => {
    const late = this.time.lateness();
    return late ? latenessText(late) : null;
  });

  /** ステップ送りの量の欄の文字（打っている途中もそのまま持つ）。 */
  protected readonly stepText = signal(String(DEFAULT_STEP_MS));
  private readonly stepMs = computed(() => {
    const text = numberText(this.stepText());
    const ms = Number(text);
    const { min_ms, max_ms } = this.controls().step;
    return /^\d+$/.test(text) && ms >= min_ms && ms <= max_ms ? ms : null;
  });
  protected readonly stepError = computed(() =>
    this.stepMs() === null
      ? $localize`ステップ送りの量は ${this.controls().step.min_ms}:min: から ${this.controls().step.max_ms}:max: までの整数（ミリ秒）にしてください。`
      : null,
  );
  /** 量の誤りを出してよいか: 欄を離れたか送った後。打ち直すと消す。 */
  protected readonly stepChecked = signal(false);
  protected readonly shownStepError = computed(() =>
    this.stepChecked() ? this.stepError() : null,
  );
  /** ステップ送りを押せるか: 正面の口が許し、量の欄が整数として読める。 */
  protected readonly canStep = computed(() => this.controls().step.ok && this.stepMs() !== null);
  /**
   * ステップ送りの説明: 押せない理由か、押せるなら注意（正面の口の `controls.step`）。shown: カードに見せるか（帯が
   * 言う接続の理由と、一時停止と再開の行と同じ文は、ボタンの説明にだけ残す）。
   */
  protected readonly stepWhy = computed(() =>
    this.cardLine(
      'step',
      this.pauseLines().map((l) => l.text),
    ),
  );
  /** 前のステップ送りで実際に進む量（ランチャーが受け付けたとき）。 */
  protected readonly stepMoved = computed(() => {
    const last = this.time.lastCommand();
    return last?.name === 'step' && last.accepted && last.ms !== null
      ? $localize`前のステップ送りで進む量: ${last.ms}:ms: ms（ランチャーが刻みの倍数に切り上げます）。`
      : '';
  });
  /**
   * 一時停止と再開の、カードに出す説明（同じ文は 1 回）。押しても何も変わらない（`paused`、`not-paused`: 状態の欄が
   * 言う）、実行がない（`not-running`）、答えを待っている（`busy`: 帯のボタンの説明が言う）は出さない。
   */
  protected readonly pauseLines = computed(() => {
    const shown: { text: string; english: boolean }[] = [];
    for (const op of ['pause', 'resume'] as const) {
      const control = this.controls()[op];
      const reason = control.reason ?? control.caution;
      if (!reason || SAID_ELSEWHERE.has(reason.code)) continue;
      const line = reasonLine(op, reason);
      if (!shown.some((s) => s.text === line.text)) shown.push(line);
    }
    return shown;
  });

  /** 変える先の速度。いまの速度から始め、いまの速度が変わったら、それに戻す。 */
  protected readonly newSpeed = linkedSignal<Speed>(() => this.time.speed() ?? 1);
  protected readonly newSpeedInvalid = signal(false);
  protected readonly canSetSpeed = computed(
    () => this.controls().speed.ok && !this.newSpeedInvalid() && this.newSpeed() !== null,
  );
  /** 「速度を変える」の説明（stepWhy と同じ決まり。ステップの行と同じ文も見せない）。 */
  protected readonly speedWhy = computed(() => {
    const step = this.stepWhy();
    const above = this.pauseLines().map((l) => l.text);
    return this.cardLine('speed', step?.shown ? [...above, step.text] : above);
  });
  /** 速度の欄を触れなくする: 実行が速度を受け付けない間（応答を待っている間は触れる）。 */
  protected readonly speedLocked = computed(() => {
    const speed = this.controls().speed;
    return !speed.ok && speed.reason?.code !== 'busy';
  });

  /** 断られた命令の理由: サーバーが断った（HTTP）か、ランチャーが受け付けなかった（停止の命令を除く）。 */
  protected readonly refusal = computed<{ text: string; english: boolean } | null>(() => {
    const error = this.time.error();
    if (error) return { text: error, english: this.time.errorFromServer() };
    const last = this.time.lastCommand();
    return last && !last.accepted && last.name !== 'stop'
      ? { text: last.message ?? last.name ?? '', english: true }
      : null;
  });

  private cardLine(
    op: 'step' | 'speed',
    above: readonly string[],
  ): { text: string; english: boolean; shown: boolean } | null {
    const control = this.controls()[op];
    const line = controlLine(op, control);
    if (!line) return null;
    const code = (control.reason ?? control.caution)?.code ?? '';
    return { ...line, shown: !BAND_SAYS.has(code) && !above.includes(line.text) };
  }

  protected typeStep(text: string): void {
    this.stepText.set(text);
    this.stepChecked.set(false);
  }

  /** 「ステップ送り」か Enter。ページは移らない（サーバーの CSP は form-action 'none'）。 */
  protected sendStep(event: Event): void {
    event.preventDefault();
    this.stepChecked.set(true);
    const ms = this.stepMs();
    if (ms !== null && this.canStep()) void this.time.step(ms);
  }

  protected setSpeed(): void {
    const speed = this.newSpeed();
    if (speed !== null && this.canSetSpeed()) void this.time.setSpeed(speed);
  }
}
