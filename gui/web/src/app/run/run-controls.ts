import { Component, computed, input, linkedSignal, model, output } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { PHASES, Speed } from '../core/api';
import { MeasurePipe } from '../core/format/measure-pipe';
import { phaseLabel } from './labels';
import { SessionView, phaseIndex } from './run-state';

/** file: 構成ファイルのとおり（ランチャーの既定）、max: 最速、real: 等速、factor: 倍率。 */
type SpeedChoice = 'file' | 'max' | 'real' | 'factor';

/** 操作: 開始、停止、速度。段階の表示、経過時間と仮想時刻。 */
@Component({
  selector: 'app-run-controls',
  imports: [
    MatButtonModule,
    MatButtonToggleModule,
    MatFormFieldModule,
    MatInputModule,
    MeasurePipe,
  ],
  templateUrl: './run-controls.html',
  styleUrl: './run-controls.css',
})
export class RunControls {
  readonly session = input.required<SessionView | null>();
  /** セッションが動いているか。 */
  readonly active = input.required<boolean>();
  /** 始められるか。始められないときは blockedBy に理由。 */
  readonly blockedBy = input.required<readonly string[]>();
  readonly pending = input.required<'start' | 'stop' | null>();
  readonly elapsedMs = input.required<number | null>();
  /** ランチャーが速度を変えられるか。できなければ速度の欄を出さない。 */
  readonly speedSupported = input(false);
  /** 倍率の範囲（サーバーが api/app で言う）。 */
  readonly speedMin = input(0.01);
  readonly speedMax = input(1000);
  /** 速度。null は構成ファイルのとおり、'max' は最速、数は倍率（1 が等速）。 */
  readonly speed = model.required<Speed>();

  readonly start = output();
  readonly stop = output();

  protected readonly canStart = computed(
    () => !this.active() && this.pending() === null && this.blockedBy().length === 0,
  );
  /** 前の実行が終わっている（「もう一度実行」）。 */
  protected readonly again = computed(() => this.session()?.result != null);
  protected readonly stopRequests = computed(() => this.session()?.stopRequests ?? 0);

  protected readonly elapsedSeconds = computed(() => (this.elapsedMs() ?? 0) / 1000);
  protected readonly simSeconds = computed(() => {
    const simMs = this.session()?.simMs ?? null;
    return simMs === null ? null : simMs / 1000;
  });

  protected readonly steps = computed(() => {
    const current = phaseIndex(this.session()?.phase ?? null);
    return PHASES.map((phase, i) => ({
      phase,
      label: phaseLabel(phase),
      state: i < current ? 'done' : i === current ? 'current' : 'todo',
    }));
  });
  protected readonly unknownPhase = computed(() => {
    const phase = this.session()?.phase ?? null;
    return phase !== null && phaseIndex(phase) < 0 ? phase : null;
  });

  protected readonly choice = linkedSignal<SpeedChoice>(() => {
    const speed = this.speed();
    return speed === null ? 'file' : speed === 'max' ? 'max' : speed === 1 ? 'real' : 'factor';
  });
  protected readonly factor = linkedSignal(() => {
    const speed = this.speed();
    return typeof speed === 'number' && speed !== 1 ? speed : 2;
  });
  protected readonly factorInvalid = computed(() => !this.validFactor(this.factor()));

  protected choose(choice: SpeedChoice): void {
    this.choice.set(choice);
    const speeds: Record<SpeedChoice, Speed> = {
      file: null,
      max: 'max',
      real: 1,
      factor: this.factor(),
    };
    this.speed.set(speeds[choice]);
  }

  protected setFactor(text: string): void {
    const value = Number(text);
    this.factor.set(value);
    if (this.choice() === 'factor' && this.validFactor(value)) {
      this.speed.set(value);
    }
  }

  private validFactor(value: number): boolean {
    return Number.isFinite(value) && value >= this.speedMin() && value <= this.speedMax();
  }
}
