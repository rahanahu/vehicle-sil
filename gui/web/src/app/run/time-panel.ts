import { Component, computed, inject, linkedSignal, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { Speed } from '../core/api';
import { MeasurePipe } from '../core/format/measure-pipe';
import { ServerInfo } from '../core/session/server-info';
import { SimulationTime } from '../core/session/simulation-time';
import { WAITING_TEXT, latenessText, speedLabel } from './labels';
import { SpeedPicker } from './speed-picker';

/** ステップ送りの量の最初の値（ミリ秒）。 */
export const DEFAULT_STEP_MS = 100;

/**
 * 実行中の時刻: 仮想時間、速さ、実際の速さ、一時停止しているか、遅れの数と、ステップ送り、速さの変更。
 * 一時停止と再開は状態の帯にある（TimeButtons）。状態はすべて SimulationTime の signal から読む。
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
      <div class="row">
        <mat-form-field class="ms" subscriptSizing="dynamic">
          <mat-label i18n>ステップ送りの量（ms）</mat-label>
          <input
            matInput
            type="text"
            inputmode="numeric"
            autocomplete="off"
            data-testid="step-ms"
            [value]="stepText()"
            [attr.aria-invalid]="stepError() !== null"
            [attr.aria-describedby]="stepError() !== null ? 'step-ms-error' : null"
            #stepInput
            (input)="stepText.set(stepInput.value)"
            (keydown.enter)="step()"
          />
        </mat-form-field>
        <button
          matButton="tonal"
          type="button"
          data-testid="step"
          [disabled]="!canStep()"
          disabledInteractive
          [attr.aria-describedby]="time.canStep() ? null : 'time-step-why'"
          (click)="step()"
          i18n
        >
          ステップ送り
        </button>
      </div>
      <p class="error" id="step-ms-error" role="alert" [class.shown]="stepError() !== null">
        {{ stepError() }}
      </p>
      <p class="hint" id="time-step-why" data-testid="step-hint">{{ stepHint() }}</p>

      <div class="row">
        <app-speed-picker
          [withFile]="false"
          [label]="speedLabelText"
          [min]="info.limits().speed_min"
          [max]="info.limits().speed_max"
          [locked]="!time.controllable()"
          [(speed)]="newSpeed"
          [(invalid)]="newSpeedInvalid"
        />
        <button
          matButton="tonal"
          type="button"
          data-testid="set-speed"
          [disabled]="!canSetSpeed()"
          disabledInteractive
          (click)="setSpeed()"
          i18n
        >
          速度を変える
        </button>
      </div>
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
  protected readonly info = inject(ServerInfo);
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
    // 日本語入力の全角の数字（１００）も受ける
    const text = this.stepText().normalize('NFKC').trim();
    const ms = Number(text);
    return /^\d+$/.test(text) && ms >= 1 && ms <= this.info.limits().step_ms_max ? ms : null;
  });
  protected readonly stepError = computed(() =>
    this.stepMs() === null
      ? $localize`ステップ送りの量は 1 から ${this.info.limits().step_ms_max}:max: までの整数（ミリ秒）にしてください。`
      : null,
  );
  protected readonly canStep = computed(
    () => this.time.canStep() && this.stepMs() !== null && this.time.sending() === null,
  );
  /** ステップ送りの説明: 押せない理由か、前のステップ送りで実際に進む量。 */
  protected readonly stepHint = computed(() => {
    const last = this.time.lastCommand();
    const moved =
      last?.name === 'step' && last.accepted && last.ms !== null
        ? $localize`前のステップ送りで進む量: ${last.ms}:ms: ms（ランチャーが刻みの倍数に切り上げます）。`
        : null;
    if (this.time.canStep()) return moved ?? '';
    const reason = !this.time.controllable()
      ? $localize`時刻の操作は、実行が動いている間だけできます。`
      : this.time.sending() !== null
        ? WAITING_TEXT
        : $localize`ステップ送りは、一時停止している間だけできます。`;
    return moved ? `${reason} ${moved}` : reason;
  });

  /** 変える先の速度。いまの速度から始め、いまの速度が変わったら、それに戻す。 */
  protected readonly newSpeed = linkedSignal<Speed>(() => this.time.speed() ?? 1);
  protected readonly newSpeedInvalid = signal(false);
  protected readonly canSetSpeed = computed(
    () =>
      this.time.controllable() &&
      !this.newSpeedInvalid() &&
      this.newSpeed() !== null &&
      this.time.sending() === null,
  );

  /** 断られた命令の理由: サーバーが断った（HTTP）か、ランチャーが受け付けなかった（停止の命令を除く）。 */
  protected readonly refusal = computed<{ text: string; english: boolean } | null>(() => {
    const error = this.time.error();
    if (error) return { text: error, english: this.time.errorFromServer() };
    const last = this.time.lastCommand();
    return last && !last.accepted && last.name !== 'stop'
      ? { text: last.message ?? last.name ?? '', english: true }
      : null;
  });

  protected step(): void {
    const ms = this.stepMs();
    if (ms !== null && this.canStep()) this.time.step(ms);
  }

  protected setSpeed(): void {
    const speed = this.newSpeed();
    if (speed !== null && this.canSetSpeed()) this.time.setSpeed(speed);
  }
}
