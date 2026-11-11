// 通しの確認用（後で捨てる）。消し方は README.md の「仮の画面」。
import { httpResource } from '@angular/common/http';
import { Component, computed, inject } from '@angular/core';
import { MatCardModule } from '@angular/material/card';
import { Health } from '../core/api';
import { MeasurePipe } from '../core/format/measure-pipe';
import { latestValue, recentValues } from '../core/streams/stream-signals';
import { Telemetry } from './telemetry';

/** グラフに残す値の数。 */
const HISTORY = 60;

@Component({
  selector: 'app-smoke-page',
  imports: [MatCardModule, MeasurePipe],
  templateUrl: './smoke-page.html',
  styleUrl: './smoke-page.css',
})
export class SmokePage {
  private readonly telemetry = inject(Telemetry);

  /** 1 回の取得は httpResource（状態と値を signal で持つ）。URL は base href からの相対。 */
  protected readonly health = httpResource<Health>(() => 'api/health');

  protected readonly connection = this.telemetry.state;

  // 流れはこの画面が生きている間だけ購読する（画面を離れると止まる）。
  private readonly latest = latestValue(this.telemetry.samples);
  private readonly history = recentValues(this.telemetry.samples, HISTORY);

  protected readonly seq = computed(() => this.latest()?.seq ?? null);
  protected readonly values = computed(() =>
    Object.entries(this.latest()?.values ?? {}).map(([name, value]) => ({ name, value })),
  );

  /** speed の直近の値を折れ線（SVG の points）にする。 */
  protected readonly speedPoints = computed(() => {
    const speeds = this.history().map((sample) => sample.values['speed'] ?? 0);
    if (speeds.length < 2) {
      return '';
    }
    const min = Math.min(...speeds);
    const span = Math.max(...speeds) - min || 1;
    return speeds
      .map((speed, i) => `${(i / (HISTORY - 1)) * 100},${40 - ((speed - min) / span) * 40}`)
      .join(' ');
  });

  /** 並びの変わり方を目で確かめるための仮のカード。 */
  protected readonly dummyCards = ['ECU A', 'ECU B', 'ECU C', 'CAN 1', 'LIN 1', 'J1939'].map(
    (title, i) => ({
      title,
      metrics: [
        { name: 'cycle [ms]', value: (i + 1) * 10 },
        { name: 'frames', value: (i + 1) * 123 },
      ],
    }),
  );
}
